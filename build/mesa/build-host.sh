#!/usr/bin/env bash
# Native Venus ICD used ONLY by the isolated transport/compute test suite.
set -euo pipefail
src="${1:?usage: build-host.sh MESA_SOURCE BUILD_DIR}"
out="${2:?usage: build-host.sh MESA_SOURCE BUILD_DIR}"
source "$(dirname "$0")/../profile.sh"
if [[ ! -f "$out/build.ninja" ]]; then
    meson setup "$out" "$src" --buildtype="$MESON_BUILD_TYPE" -Db_ndebug=if-release \
        -Dplatforms= -Dvulkan-drivers=virtio -Dgallium-drivers= -Dshared-glapi=disabled \
        -Dgles1=disabled -Dgles2=disabled -Degl=disabled -Dglx=disabled -Dopengl=false
fi
meson configure "$out" --buildtype="$MESON_BUILD_TYPE" -Db_ndebug=if-release
meson compile -C "$out" -j "$JOBS"
