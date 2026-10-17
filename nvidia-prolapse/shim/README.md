# libnvglibc

Lets NVIDIA's unmodified, glibc-linked Vulkan ICD closure (`libEGL_nvidia.so.0`,
`libnvidia-glsi`, `-eglcore`, `-gpucomp`, `-glvkspirv`) load in a bionic
(Android 13, x86_64) process. It builds six small "front" libraries that carry
the glibc SONAMEs (`libc.so.6`, `libm.so.6`, `libpthread.so.0`, `librt.so.1`,
`libdl.so.2`, `ld-linux-x86-64.so.2`) and export every imported name
unversioned, plus one core library (`libnvglibc.so`) that holds the translators.
Anything the shims cannot honour aborts with a `libnvglibc:` message that names
the symbol or value.

How and why it works: [DESIGN.md](DESIGN.md). Per-name dispositions:
[dispositions.tsv](dispositions.tsv).

## Build

Needs NDK r30 at `/opt/android-ndk` (override with `NDK=`), host `gcc` and
`python3`, and the NVIDIA driver loaded on the host: the driver version is read
from `/proc/driver/nvidia/version` and the blobs are read from `/usr/lib`
(override with `BLOBDIR=`) for the import census. No NVIDIA file is copied.

    make            # builds out/ and runs every check below
    make check-abi  # only the host-side glibc layout check
    make clean

## What `make` checks (any failure stops the build)

- Every glibc import of the five blobs has a row in `dispositions.tsv`, and
  every bionic target exists in the NDK API-33 `libc`/`libm`/`libdl` stubs.
- `tools/gen-constants.py`: glibc→bionic tables for `_SC_*` and `nl_item`;
  `LC_*`, `LC_*_MASK`, `RTLD_*`, `SA_*`, `SIG_*`, `PTHREAD_MUTEX_*`,
  `PTHREAD_PRIO_*`, `CLOCK_*` must be identical where a trampoline passes them on.
- `tools/check-artifacts.py`: exact SONAMEs; fronts need only `libnvglibc.so`;
  every name exported GLOBAL DEFAULT unversioned; `stdin`/`stdout`/`stderr`
  are 8-byte objects; fronts reference only `nvg_*`; each front defines the
  eleven `LIBC*` dummy versions; the core defines only `nvg_*`.
- `check-abi`: `core/glibc_abi.h` matches `tools/abi-check.c` built against the
  host glibc.

## Runtime environment

- `NVGLIBC_GLIBC_VERSION` (required when eglcore asks): host glibc version, e.g. `2.44`.
- `NVGLIBC_TRACE=1` (optional): logs dlopen/dlvsym, glibc static mutex
  conversions, front binding and default-environment lookups to stderr and logcat.
- `nvglibc-default-env` (optional file next to `libnvglibc.so`): `NAME=VALUE` defaults that the
  blobs' `getenv` returns only for names absent from the real environment (`${ORIGIN}` = that
  directory). `make test` runs the parser's host test.

`dispositions.tsv` is regenerated from a per-import table by
`make dispositions IMPORTS_TSV=<file>`.
