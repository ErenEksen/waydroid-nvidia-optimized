# Shared, explicit build policy. Source from the component build scripts.
: "${BUILD_PROFILE:=release}"
case "$BUILD_PROFILE" in
    release) MESON_BUILD_TYPE=release ;;
    diagnostic) MESON_BUILD_TYPE=debugoptimized ;;
    *) echo "BUILD_PROFILE must be release or diagnostic" >&2; exit 2 ;;
esac
: "${JOBS:=$(nproc)}"
[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || { echo 'JOBS must be a positive integer' >&2; exit 2; }
# Leave capacity for the desktop; callers can explicitly request fewer jobs.
(( JOBS <= 12 )) || JOBS=12
export JOBS
