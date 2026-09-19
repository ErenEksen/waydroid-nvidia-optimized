#!/usr/bin/env bash
# env.sh — single source of truth for the dev loop's tree locations, build
# targets, deploy destinations and patch-regen anchors. Sourced by every dev/
# script. Everything is overridable from the environment, e.g.
#   WAYDROID_SRC=/somewhere/else dev/restart
#
# The three trees this repo orchestrates (see docs/dev-workflow.md):
#   REPO         this repo — AUR source of truth (patches/ + src/ + build glue)
#   WAYDROID_SRC the runtime waydroid python checkout (waydroid.py, live lxc.py)
#   WNV/*        native component build trees (mesa / virgl / hwcomposer / angle)

set -euo pipefail

# --- this repo (dir containing dev/); overridable for testing ---
: "${REPO:=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"

# --- external trees ---
: "${WNV:=$REPO/.work}"
: "${WAYDROID_SRC:=}"
# A checkout is opt-in; normal installations use the packaged executable.
if [[ -n "$WAYDROID_SRC" ]]; then
    WAYDROID=(python3 "$WAYDROID_SRC/waydroid.py")
else
    WAYDROID=("${WAYDROID_BIN:-/usr/bin/waydroid}")
fi

: "${MESA_TREE:=$WNV/mesa}"
: "${MESA_BUILD_X86_64:=${MESA_BUILD:-$MESA_TREE/build-android-x86_64}}"
: "${MESA_BUILD_X86:=$MESA_TREE/build-android-x86}"
# Backward-compatible name used by older local helpers.
: "${MESA_BUILD:=$MESA_BUILD_X86_64}"
: "${VIRGL_TREE:=$WNV/virglrenderer}"
: "${VIRGL_BUILD:=$VIRGL_TREE/build}"
: "${HWC_TREE:=$WNV/hwcomposer-src}"
: "${HWC_BUILD:=$WNV/hwc-build}"
: "${ANGLE_TREE:=$WNV/angle-src}"
: "${ANGLE_OUT_X86_64:=$ANGLE_TREE/out/AndroidX64}"
: "${ANGLE_OUT_X86:=$ANGLE_TREE/out/AndroidX86}"

# --- toolchain ---
source "$REPO/packaging/ci/pins.env"
source "$REPO/packaging/ci/hwc-pins.env"
: "${NDK:=$WNV/android-ndk-$NDK_VERSION}"
: "${MINIGBM:=$WNV/minigbm}"
if [[ -d "$WNV/link-rootfs" ]]; then : "${ROOTFS:=$WNV/link-rootfs}"; fi
export MINIGBM
[[ -z "${ROOTFS:-}" ]] || export ROOTFS
# Do not put the build venv on PATH here. Packaged Waydroid uses
# /usr/bin/env python3 and needs the distribution's dbus/PyGObject modules.
# Only dev/build selects the isolated Python build tools.
export NDK WNV
: "${NDK_BIN:=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin}"
: "${STRIP:=$NDK_BIN/llvm-strip}"

# --- runtime ---
: "${LXC:=-P /var/lib/waydroid/lxc -n waydroid}"
: "${VENUS_UNIT:=wd-venus.service}"
: "${CONTAINER_UNIT:=waydroid-container.service}"
: "${SESSION_UNIT:=wd-session.service}"
: "${DEPLOY:=/usr/local/sbin/wd-deploy}"

# --- patch-regen anchors (verified ancestors; see dev/sync-patches) ---
: "${MESA_BASE:=$MESA_SHA}"
: "${VIRGL_BASE:=$VIRGL_SHA}"
: "${HWC_BASE:=$HWC_SHA}"
: "${WAYDROID_BASE:=$WAYDROID_SHA}"

# --- helpers ---
say()  { printf '\033[1;36m== %s\033[0m\n' "$*"; }
warn() { printf '\033[1;33m!! %s\033[0m\n' "$*" >&2; }
die()  { printf '\033[1;31mxx %s\033[0m\n' "$*" >&2; exit 1; }

# guest shell (root inside the container), env cleaned so exec sh never fails.
# stdio goes through pipes, NOT the caller's fds: lxc-attach chowns its std
# fds to the container root (attach.c fix_stdio_permissions), which turns any
# redirect-target file into an unreadable root-owned 600 file.
guest() {
    sudo -n lxc-attach $LXC --clear-env -v PATH=/system/bin -- /system/bin/sh -c "$*" \
        < /dev/null 2> >(cat >&2) | cat
}

# Use the caller's authenticated terminal; do not install sudoers rules.
have_sudo() { sudo -n lxc-info --version >/dev/null 2>&1; }
need_sudo() { have_sudo || die "sudo -n unavailable — run this tool from your own terminal after sudo -v; do not add broad NOPASSWD rules"; }
