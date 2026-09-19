"""Keep rendering work, launch waits, and actual presentation separate."""
import math
import re

SENTINEL = (1 << 63) - 1


def percentile(values, p):
    if not values:
        return None
    ordered = sorted(values)
    return ordered[max(0, min(len(ordered) - 1, math.ceil(len(ordered) * p / 100) - 1))]


def summary(values):
    return {"samples": len(values), **{f"p{p}_ms": percentile(values, p) for p in (50, 95, 99)},
            "max_ms": max(values) if values else None,
            "over_50ms": sum(v > 50 for v in values)}


def framestats(text, start_ns=0, end_ns=SENTINEL, active_ranges=None):
    blocks, header, records = False, None, []
    invalid = flagged = 0
    by_frame = {}
    window = "default"
    for line in text.splitlines():
        line = line.strip()
        if line.startswith("Window:"):
            window = line[7:].strip()
        elif line == "---PROFILEDATA---":
            blocks = not blocks
            header = None
        elif blocks and line.startswith("Flags,"):
            header = line.rstrip(",").split(",")
        elif blocks and header and re.match(r"^\d+,", line):
            try:
                row = dict(zip(header, map(int, line.rstrip(",").split(","))))
                intended, completed = row["IntendedVsync"], row["FrameCompleted"]
                if row["Flags"] != 0:
                    flagged += 1
                    # Keep first-draw flagged frames: they are the point of a startup test.
                if not (0 < intended < SENTINEL and intended <= completed < SENTINEL):
                    invalid += 1
                    continue
                if not start_ns <= intended <= end_ns:
                    continue
                key = (window, intended)
                previous = by_frame.get(key)
                new_present = row.get("DisplayPresentTime", 0)
                old_present = previous.get("DisplayPresentTime", 0) if previous else 0
                new_valid = 0 < new_present < SENTINEL and intended <= new_present <= end_ns
                old_valid = 0 < old_present < SENTINEL and intended <= old_present <= end_ns
                if previous is None or (new_valid, completed) >= (old_valid, previous["FrameCompleted"]):
                    row["window"] = window
                    by_frame[key] = row
            except (ValueError, KeyError):
                invalid += 1
    records = list(by_frame.values())
    flagged = sum(r["Flags"] != 0 for r in records)
    work = [(r["FrameCompleted"] - r["IntendedVsync"]) / 1e6 for r in records]
    # Deliberately DO NOT substitute FrameCompleted for display presentation.
    streams = {}
    for r in records:
        timestamp = r.get("DisplayPresentTime", 0)
        if 0 < timestamp < SENTINEL and r["IntendedVsync"] <= timestamp <= end_ns:
            streams.setdefault(r["window"], set()).add(timestamp)
    present = sorted({t for times in streams.values() for t in times})
    missing_present = sum(not (0 < r.get("DisplayPresentTime", 0) < SENTINEL and
                               r["IntendedVsync"] <= r["DisplayPresentTime"] <= end_ns) for r in records)
    intervals = []
    excluded = 0
    for times in streams.values():
        ordered = sorted(times)
        for a, b in zip(ordered, ordered[1:]):
            # Do not score deliberate pauses between input commands as jank.
            # A stall within an active range is NEVER clipped or discarded.
            if active_ranges is not None and not any(lo <= a < b <= hi for lo, hi in active_ranges):
                excluded += 1
            else:
                intervals.append((b-a)/1e6)
    return {"hwui_work": summary(work), "hwui_work_ms": work,
            "presentation": summary(intervals), "presentation_intervals_ms": intervals,
            "presentation_source": "gfxinfo.DisplayPresentTime" if present else None,
            "first_present_after_request_ms": (present[0] - start_ns) / 1e6 if present and start_ns else None,
            "invalid_rows": invalid, "flagged_rows": flagged,
            "presentation_missing_frames": missing_present, "presentation_streams": len(streams), "idle_intervals_excluded": excluded,
            "complete": bool(work and intervals and not missing_present)}


def display_size(text):
    sizes = re.findall(r"(?:Physical|Override) size:\s*(\d+)x(\d+)", text)
    if not sizes:
        raise ValueError("wm size did not report a display size")
    w, h = map(int, sizes[-1])
    if w < 64 or h < 64:
        raise ValueError("invalid display dimensions")
    return w, h


def sf_period_ns(text):
    # SurfaceFlinger --latency's first line is the nominal period even when
    # its old frame table is empty on Android 13.
    first = text.strip().splitlines()[0] if text.strip() else ""
    if first.isdigit() and 1_000_000 <= int(first) <= 100_000_000:
        return int(first)
    raise ValueError("SurfaceFlinger did not report a refresh period")


def activity_launch(text):
    if not re.search(r"^Status:\s*ok\s*$", text, re.M):
        raise ValueError("am start -W did not report Status: ok")
    fields = {}
    for k, v in re.findall(r"^(ThisTime|TotalTime|WaitTime):\s*(\d+)", text, re.M):
        fields[k] = int(v)
    return {"source": "am start -W (NOT a presentation timestamp)", **fields}


def focused_bounds(text, package):
    """Use Android's actual visible app window, not assumed screen coordinates."""
    focus = re.search(r"mCurrentFocus=Window\{([^}]+)\}", text)
    if not focus or package not in focus.group(1):
        raise ValueError("requested package is not the focused Android window")
    token = focus.group(1).split()[0]
    for section in re.split(r"(?=Window #\d+ Window\{)", text):
        if not re.match(r"Window #\d+ Window\{" + re.escape(token) + r"\b", section):
            continue
        match = re.search(r"(?:mFrame|\bframe)=\[(-?\d+),\s*(-?\d+)\]\[(-?\d+),\s*(-?\d+)\]", section)
        if match:
            l, t, r, b = map(int, match.groups())
            if r-l >= 64 and b-t >= 64:
                return l, t, r, b
    raise ValueError("focused app window bounds unavailable")
