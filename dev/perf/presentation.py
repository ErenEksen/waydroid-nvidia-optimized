"""Host wp_presentation data, never substituted for Android frame production."""
import csv
import io
from .metrics import summary


def presentation(text, start_ns=0, end_ns=(1 << 63)-1):
    records, discarded, invalid = [], 0, 0
    for row in csv.DictReader(io.StringIO(text)):
        try:
            if row['event'] == 'discarded':
                discarded += 1
                continue
            r = {key: int(row[key]) for key in ('submit_ns', 'present_ns', 'refresh_ns', 'sequence', 'flags', 'output_id')}
            if row['event'] != 'presented' or r['present_ns'] <= 0 or r['refresh_ns'] <= 0:
                invalid += 1
            elif start_ns <= r['submit_ns'] <= r['present_ns'] <= end_ns and r['output_id']:
                records.append(r)
        except (ValueError, KeyError, TypeError):
            invalid += 1
    outputs = {r['output_id'] for r in records}
    intervals, missed, expected = [], 0, 0
    estimated = False
    for a, b in zip(records, records[1:]):
        if a['output_id'] != b['output_id']:
            continue
        delta = b['present_ns'] - a['present_ns']
        sequence = b['sequence'] - a['sequence']
        if a['sequence'] == 0 and b['sequence'] == 0 and delta > 0:
            # Some compositors supply presentation times but no refresh counter.
            # Keep this explicitly an estimate, never invent a hardware counter.
            sequence = max(1, int(delta/b['refresh_ns'] + 0.5))
            estimated = True
        if delta <= 0 or sequence <= 0:
            invalid += 1
            continue
        intervals.append(delta/1e6)
        missed += max(0, sequence - 1)
        expected += sequence
    return {'source': 'host.wp_presentation', 'intervals': summary(intervals),
            'intervals_ms': intervals, 'missed_refreshes': missed, 'expected_refreshes': expected,
            'missed_ratio': missed/expected if expected else None,
            'missed_source': 'timestamp_estimate' if estimated else 'presentation_sequence',
            'discarded': discarded, 'invalid': invalid, 'output_count': len(outputs),
            'complete': bool(intervals and len(outputs) == 1 and 0 not in outputs and not invalid)}
