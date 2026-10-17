#!/usr/bin/env bash
# g1-check.sh — gate G1 host-side half: is the guest probe visible to nvidia-smi?
# Usage: sudo -n ./probe/g1-check.sh <guest-pid>
# Maps the guest (container) PID to the host PID via /proc/*/status NSpid, then asks nvidia-smi
# for its compute apps. Exit 0 only when the host PID appears there. Reads nothing by node path.
set -euo pipefail
gpid=${1:?guest pid}
hpid=
for s in /proc/[0-9]*/status; do
    # NSpid: <host pid> <pid in child ns> ... — the last field is the innermost namespace
    if awk -v g="$gpid" '/^NSpid:/{ if (NF>=3 && $NF==g) found=1 } END{exit !found}' "$s" 2>/dev/null; then
        hpid=${s#/proc/}; hpid=${hpid%/status}; break
    fi
done
[ -n "$hpid" ] || { echo "g1-check: no host process has NSpid ending in $gpid (is the probe still running? use VKCOMPUTE_HOLD=1)" >&2; exit 2; }
echo "guest pid $gpid = host pid $hpid ($(tr '\0' ' ' < /proc/$hpid/cmdline))"
echo "fds on NVIDIA nodes: $(ls -l /proc/$hpid/fd 2>/dev/null | grep -c nvidia)"
echo "--- nvidia-smi compute apps:"
nvidia-smi --query-compute-apps=pid,process_name,used_memory --format=csv
if nvidia-smi --query-compute-apps=pid --format=csv,noheader | grep -qx "$hpid"; then
    echo "G1 host half: PASS (host pid $hpid listed by nvidia-smi)"
else
    echo "G1 host half: FAIL (host pid $hpid not listed by nvidia-smi)"; exit 1
fi
