#!/bin/bash
# List every project of a synced LineageOS/Waydroid tree whose manifest revision is a branch, compare the
# revision the tree was synced at (repo's m/ ref) with the remote branch head today, and for moved branches
# resolve the last first-parent commit at or before CUTOFF (an image's build date, UTC).
# Usage: project-delta.sh TREE CUTOFF OUTDIR      (OUTDIR absolute; read-only on the tree except `git fetch` of moved
#        branches into refs/remotes/delta/head). "synced sha" = $REPO_LREV, the remote-tracking ref: a manual
#        `git fetch <remote> <branch>` in a project beforehand moves it (that happened to the Waydroid projects on
#        2026-10-16; harmless there because their heads are older than the cutoff).
# Output: OUTDIR/projects.tsv (path project remote rrev synced_sha), OUTDIR/heads.tsv (path head_today),
#         OUTDIR/moved.tsv (path rrev synced_sha cutoff_sha head_today n_commits_to_cutoff)
set -uo pipefail
TREE=$1 CUTOFF=$2 OUT=$3
mkdir -p "$OUT"; cd "$TREE" || exit 1
repo forall -c 'printf "%s\t%s\t%s\t%s\t%s\n" "$REPO_PATH" "$REPO_PROJECT" "$(git remote get-url "$REPO_REMOTE")" "$REPO_RREV" "$REPO_LREV"' \
  > "$OUT/projects.tsv" 2>"$OUT/forall.err"
echo "projects: $(wc -l < "$OUT/projects.tsv")"
# branch revisions only: tags (refs/tags/...) and full SHAs cannot move
awk -F'\t' '$4 !~ /^refs\/tags\// && $4 !~ /^[0-9a-f]{40}$/' "$OUT/projects.tsv" > "$OUT/branch-projects.tsv"
echo "branch-tracked: $(wc -l < "$OUT/branch-projects.tsv")"
: > "$OUT/heads.tsv"
while IFS=$'\t' read -r path proj url rrev lrev; do
  ref=$rrev; case "$ref" in refs/*) ;; *) ref=refs/heads/$ref ;; esac
  printf '%s\t%s\t%s\n' "$path" "$url" "$ref"
done < "$OUT/branch-projects.tsv" |
  xargs -P8 -L1 -d '\n' bash -c 'IFS=$'"'"'\t'"'"' read -r p u r <<< "$0"; h=$(timeout 60 git ls-remote "$u" "$r" | awk "{print \$1}" | head -1); printf "%s\t%s\n" "$p" "${h:-ERR}"' \
  >> "$OUT/heads.tsv"
sort -o "$OUT/heads.tsv" "$OUT/heads.tsv"
echo "ls-remote errors: $(grep -c $'\tERR' "$OUT/heads.tsv")"
: > "$OUT/moved.tsv"
join -t $'\t' <(sort "$OUT/branch-projects.tsv") "$OUT/heads.tsv" | while IFS=$'\t' read -r path proj url rrev lrev head; do
  [ "$head" = "$lrev" ] && continue
  [ "$head" = ERR ] && { printf '%s\t%s\t%s\tERR\tERR\t-\n' "$path" "$rrev" "$lrev" >> "$OUT/moved.tsv"; continue; }
  ref=$rrev; case "$ref" in refs/*) ;; *) ref=refs/heads/$ref ;; esac
  timeout 600 git -C "$path" fetch -q "$url" "$ref:refs/remotes/delta/head" </dev/null 2>>"$OUT/fetch.err"
  cut=$(git -C "$path" rev-list -1 --first-parent --before="$CUTOFF" refs/remotes/delta/head)
  n=$(git -C "$path" rev-list --count "$lrev..$cut" 2>/dev/null || echo ?)
  printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$path" "$rrev" "$lrev" "$cut" "$head" "$n" >> "$OUT/moved.tsv"
done
echo "moved since sync: $(wc -l < "$OUT/moved.tsv")"
