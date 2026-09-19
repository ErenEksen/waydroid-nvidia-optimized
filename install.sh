#!/usr/bin/env bash
# Existing NVIDIA-stack upgrade; never initialize Android or replace user data.
set -euo pipefail
REPO=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
mode=--apply
case "${1:-}" in
  '') ;;
  --check) mode=--check ;;
  --help|-h) printf '%s\n' 'Usage: ./install.sh [--check]' 'Downloads the pinned optimized bundle; verifies it before requesting sudo.' 'Requires an initialized upstream waydroid-nvidia installation (not stock Waydroid).' 'Run as your desktop user. --check downloads/verifies without installing.'; exit 0 ;;
  *) echo 'Usage: ./install.sh [--check]' >&2; exit 2 ;;
esac
(( $# <= 1 )) || { echo 'Too many arguments' >&2; exit 2; }
(( EUID != 0 )) || { echo 'Run as your desktop user, not sudo ./install.sh' >&2; exit 1; }
for tool in curl python3 sha256sum mktemp; do
  command -v "$tool" >/dev/null || { echo "Missing dependency: $tool" >&2; exit 1; }
done
if [[ "$mode" == --apply ]]; then
  [[ $(uname -m) == x86_64 ]] || { echo 'Only x86_64 Linux hosts are supported' >&2; exit 1; }
  [[ -x /usr/bin/waydroid && -x /usr/lib/waydroid-nvidia/virgl_test_server && -d /var/lib/waydroid/nv/guest ]] || {
    echo 'First install and initialize upstream waydroid-nvidia (see README). Stock Waydroid alone is not enough.' >&2; exit 1;
  }
fi
# This file is versioned with the installer; never trust a checksum from the download alone.
read -r expected asset < "$REPO/packaging/releases/optimized.sha256"
[[ "$expected" =~ ^[0-9a-f]{64}$ && "$asset" == waydroid-nvidia-optimized-perf-v1.tar.gz ]] || { echo 'Invalid pinned release metadata' >&2; exit 1; }
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
url="https://github.com/ErenEksen/waydroid-nvidia-optimized/releases/download/perf-v1/$asset"
curl --fail --location --proto '=https' --proto-redir '=https' --retry 2 --connect-timeout 15 --max-time 300 --output "$work/$asset" "$url"
printf '%s  %s\n' "$expected" "$work/$asset" | sha256sum --check --status
python3 "$REPO/packaging/releases/extract-bundle.py" "$work/$asset" "$work/bundle"
"$REPO/dev/install-bundle" --check "$work/bundle"
if [[ "$mode" == --apply ]]; then
  echo 'This restarts Waydroid. Save your game first. Existing components are backed up; app data is preserved.'
  sudo -v
  "$REPO/dev/install-bundle" --apply "$work/bundle"
else
  echo 'Release verified. Nothing installed or restarted.'
fi
