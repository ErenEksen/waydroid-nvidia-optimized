#!/usr/bin/env bash
# Install waydroid-nvidia-optimized from this checkout; no other waydroid-nvidia installation is needed.
#   ./install.sh [--check] [--no-deps] [--from DIR]   then: sudo waydroid-nvidia-setup; waydroid session start
#   ./install.sh --uninstall
# --check downloads and verifies without root and installs nothing. Details: packaging/install.sh --help
set -euo pipefail
REPO=$(cd -- "$(dirname -- "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)
case " $* " in
    *' --check '*|*' -h '*|*' --help '*) exec "$REPO/packaging/install.sh" "$@" ;;
esac
(( EUID == 0 )) && exec "$REPO/packaging/install.sh" "$@"
exec sudo "$REPO/packaging/install.sh" "$@"
