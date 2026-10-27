#!/bin/bash
# For every branch-tracked project (project-delta.sh branch-projects.tsv): the last first-parent commit at or
# before CUTOFF (OUT must be an absolute path) on the remote branch (refs/remotes/delta/head when project-delta.sh fetched it because it moved,
# else the synced sha, which IS the remote head today). Prints path<TAB>sha<TAB>commit date.
set -euo pipefail
TREE=$1 CUTOFF=$2 OUT=$3
cd "$TREE"
declare -A moved
while IFS=$'\t' read -r path _; do moved[$path]=1; done < "$OUT/moved.tsv"
while IFS=$'\t' read -r path proj url rrev lrev; do
  if [ -n "${moved[$path]:-}" ]; then tip=refs/remotes/delta/head; else tip=$lrev; fi
  sha=$(git -C "$path" rev-list -1 --first-parent --before="$CUTOFF" "$tip")
  [ -n "$sha" ] || { echo "no commit before $CUTOFF in $path" >&2; exit 1; }
  printf '%s\t%s\t%s\n' "$path" "$sha" "$(git -C "$path" log -1 --format=%cI "$sha")"
done < "$OUT/branch-projects.tsv" > "$OUT/cutoff.tsv"
echo "cutoff shas: $(wc -l < "$OUT/cutoff.tsv")"
