# libnvglibc

Lets NVIDIA's unmodified, glibc-linked Vulkan ICD closure (`libEGL_nvidia.so.0`,
`libnvidia-glsi`, `-eglcore`, `-glvkspirv`, and `-gpucomp` from driver 550 on) load
in a bionic (Android 13, x86_64 and x86) process, for every driver branch from 535 to
615. It builds six small "front" libraries that carry
the glibc SONAMEs (`libc.so.6`, `libm.so.6`, `libpthread.so.0`, `librt.so.1`,
`libdl.so.2`, `ld-linux-x86-64.so.2`) and export every imported name
unversioned, plus one core library (`libnvglibc.so`) that holds the translators.
Anything the shims cannot honour aborts with a `libnvglibc:` message that names
the symbol or value.

How and why it works: [DESIGN.md](DESIGN.md). Per-name dispositions:
[dispositions.tsv](dispositions.tsv).

## Build

Needs NDK r30 at `/opt/android-ndk` (override with `NDK=`), host `gcc` and
`python3`. The fronts export the union of the glibc imports of every supported
driver branch, read from the reference lists in `imports/<version>-<arch>.tsv`
(cut from each branch's own libraries with `tools/import-census.py --allow-unused`;
no NVIDIA file is kept). When the NVIDIA driver is loaded on the host, its own
libraries are found the way the dynamic loader finds them (`tools/find-driver.py`:
the loader cache, at the kernel module's version) and must be covered too. A build
machine without the driver says why: `make REFERENCE_ONLY="<reason>"`.

    make            # builds out/ and runs every check below
    make check-abi  # only the host-side glibc layout check
    make clean

## What `make` checks (any failure stops the build)

- Every glibc import of every reference list (and of the host driver) has a row
  in `dispositions.tsv`, every row is imported by at least one of them, and
  every bionic target exists in the NDK API-33 `libc`/`libm`/`libdl` stubs.
- `tools/gen-constants.py`: glibc→bionic tables for `_SC_*` and `nl_item`;
  `LC_*`, `LC_*_MASK`, `RTLD_*`, `SA_*`, `SIG_*`, `PTHREAD_MUTEX_*`,
  `PTHREAD_PRIO_*`, `PTHREAD_*_SCHED`, `PTHREAD_CREATE_*`, `SCHED_*`, `CLOCK_*` must be
  identical where a trampoline passes them on.
- `tools/check-artifacts.py`: exact SONAMEs; fronts need only `libnvglibc.so`;
  every name exported GLOBAL DEFAULT unversioned; `stdin`/`stdout`/`stderr`
  are 8-byte objects; fronts reference only `nvg_*`; each front defines the
  eleven `LIBC*` dummy versions; the core defines only `nvg_*`.
- `check-abi`: `core/glibc_abi.h` matches `tools/abi-check.c` built against the
  host glibc.

## Checking an installed driver against a build

    python3 -I tools/import-census.py --arch x86_64 --dispositions dispositions.tsv \
        --covered-by gen/census.tsv $(python3 -I tools/find-driver.py --arch x86_64)

fails, naming each import, when the installed driver imports a glibc name the build
does not export (a driver release newer than the reference lists). The package's
setup runs this before it switches to glibc-shim mode.

## Runtime environment

- `NVGLIBC_GLIBC_VERSION` (required when eglcore asks): host glibc version, e.g. `2.44`.
- `NVGLIBC_TRACE=1` (optional): logs dlopen/dlvsym, glibc static mutex
  conversions, front binding and default-environment lookups to stderr and logcat.
- `nvglibc-default-env` (optional file next to `libnvglibc.so`): `NAME=VALUE` defaults that the
  blobs' `getenv` returns only for names absent from the real environment (`${ORIGIN}` = that
  directory). `make test` runs the parser's host test.

`dispositions.tsv` is regenerated from a per-import table by
`make dispositions IMPORTS_TSV=<file>`.
