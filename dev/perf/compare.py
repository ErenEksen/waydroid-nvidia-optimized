"""Acceptance requires comparable and complete presentation measurements."""
from .metrics import percentile


def compare(before, after):
    errors = []
    for name, report in (("before", before), ("after", after)):
        if not report.get("complete") or not report.get("runs"):
            errors.append(f"{name}: incomplete capture")
    for field in ("schema_version", "package", "mode", "requested_runs", "duration_s", "desktop_output"):
        if before.get(field) != after.get(field):
            errors.append(f"mismatched {field}")
    if before.get("schema_version") != 2 or after.get("schema_version") != 2:
        errors.append("unsupported capture schema")
    for key in ("kernel", "gpu", "power_profile"):
        b, a = before.get("metadata", {}).get(key), after.get("metadata", {}).get(key)
        if not b or not a or b != a:
            errors.append(f"unknown/different controlled metadata: {key}")
    for name, report in (("before", before), ("after", after)):
        if len(report.get("runs", [])) != report.get("requested_runs"):
            errors.append(f"{name}: not all requested runs captured")
        if not all(r.get("complete") and r.get("desktop", {}).get("complete") for r in report.get("runs", [])):
            errors.append(f"{name}: incomplete per-run measurements")
    br, ar = before.get("runs", []), after.get("runs", [])
    if len(br) != len(ar):
        errors.append("mismatched run counts")
    for b, a in zip(br, ar):
        if b.get("size") != a.get("size") or b.get("refresh_period_ns") != a.get("refresh_period_ns"):
            errors.append("mismatched display size/refresh")
    if errors:
        return {"status": "incomplete", "errors": errors}
    b = [v for r in br for v in r["presentation_intervals_ms"]]
    a = [v for r in ar for v in r["presentation_intervals_ms"]]
    if not b or not a:
        return {"status": "incomplete", "errors": ["missing actual presentation intervals"]}
    p99_before, p99_after = percentile(b, 99), percentile(a, 99)
    cold = after["mode"] != "warm"
    # Rates instead of absolute counts: a faster run can produce more frames.
    old_stalls = sum(v > 50 for v in b) / len(b)
    new_stalls = sum(v > 50 for v in a) / len(a)
    criteria = {"p99": p99_after <= (0.7 * p99_before if cold else 2 * ar[0]["refresh_period_ns"] / 1e6),
                "over_50ms_rate": new_stalls <= old_stalls * (0.5 if cold else 1)}
    bd = [r["desktop"] for r in br]
    ad = [r["desktop"] for r in ar]
    if {r["missed_source"] for r in bd} != {r["missed_source"] for r in ad}:
        return {"status": "incomplete", "errors": ["different desktop missed-refresh measurement sources"]}
    old_desktop = sum(r["missed_refreshes"] for r in bd)/sum(r["expected_refreshes"] for r in bd)
    new_desktop = sum(r["missed_refreshes"] for r in ad)/sum(r["expected_refreshes"] for r in ad)
    criteria["desktop_missed_ratio"] = new_desktop <= old_desktop * 1.05
    return {"status": "pass" if all(criteria.values()) else "fail", "criteria": criteria,
            "before_desktop_missed_ratio": old_desktop, "after_desktop_missed_ratio": new_desktop,
            "before_p99_ms": p99_before, "after_p99_ms": p99_after,
            "before_over_50ms_rate": old_stalls, "after_over_50ms_rate": new_stalls,
            "note": "UI + desktop pacing gates only; correctness, 30-minute stability and synthetic throughput remain separate acceptance gates."}
