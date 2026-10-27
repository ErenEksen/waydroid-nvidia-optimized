#!/usr/bin/env python3
"""Generate shim/dispositions.tsv from a per-import table.

Input is imports-<ver>.tsv (one row per glibc import of the NVIDIA libraries, with a
notes column); output is dispositions.tsv with the note column dropped.
Run with `python3 -I`.

Output columns: name  disposition  target  runtime
  disposition  verbatim from the input table (except OVERRIDES below)
  target       bionic symbol name for identical / renamed trampolines and for
               data symbols; nvg_<name> for names the core translates or
               implements
  runtime      libpthread.so.0 for the names eglcore resolves at runtime with
               dlvsym on that handle (they are in no import table); '-' otherwise
"""
import csv
import sys

# Names whose glibc call is the bionic function under another name, with an
# identical C ABI (DESIGN.md "Trampolines", "renamed identical"; the locale set
# is only valid while gen-constants.py proves LC_*_MASK identical, which the
# build enforces).
RENAMES = {
    "__errno_location": "__errno",
    "strerror_r": "__gnu_strerror_r",
    "_IO_getc": "getc",
    "_IO_putc": "putc",
    "__isoc99_sscanf": "sscanf",
    "__isoc99_fscanf": "fscanf",
    "__pthread_key_create": "pthread_key_create",
    "__strdup": "strdup",
    "__duplocale": "duplocale",
    "__freelocale": "freelocale",
    "__uselocale": "uselocale",
    "__strtod_l": "strtod_l",
    "__strtof_l": "strtof_l",
    "__strcoll_l": "strcoll_l",
    "__strxfrm_l": "strxfrm_l",
    "__strftime_l": "strftime_l",
    "__wcscoll_l": "wcscoll_l",
    "__wcsxfrm_l": "wcsxfrm_l",
    "__wcsftime_l": "wcsftime_l",
    "__iswctype_l": "iswctype_l",
    "__wctype_l": "wctype_l",
    "__towlower_l": "towlower_l",
    "__towupper_l": "towupper_l",
}

# FILE* data objects: the front exports its own 8-byte variable, filled from
# bionic's at init. Target is bionic's object of the same name.
DATA = {"stdin", "stdout", "stderr"}

# Rows where DESIGN.md (frozen) requires a translator but the input table says
# otherwise. Each entry carries its reason; keep this list empty when the state
# table agrees.
# __newlocale is NOT a rename: glibc treats category_mask == 1<<LC_ALL (64) as "all
# categories" (locale/newlocale.c) and libstdc++'s _S_create_c_locale passes exactly that;
# bionic rejects it with EINVAL. core/misc.c nvg___newlocale maps it (P1 guest finding).
OVERRIDES = {
    # DESIGN.md translator table: strip RTLD_DEEPBIND (glibc 0x8). Bionic's
    # dlopen rejects any flag outside NOW|LAZY|LOCAL|GLOBAL|NODELETE|NOLOAD
    # (android13-release linker.cpp:2087 "invalid flags to dlopen").
    "dlopen": "translate",
}

# eglcore: dlopen("libpthread.so.0") + dlvsym(..., "GLIBC_*") (abi-contract.md §6).
RUNTIME_LIBPTHREAD = [
    "pthread_equal", "pthread_setname_np", "pthread_getcpuclockid",
    "sem_timedwait", "pthread_rwlock_tryrdlock", "pthread_rwlock_trywrlock",
    "pthread_setaffinity_np",
    # found by the first guest run (libEGL_nvidia aborts "Unable to find symbol" without them)
    "pthread_exit", "sem_wait",
]


def main(argv):
    if len(argv) != 3:
        sys.exit("usage: gen-dispositions.py <state imports tsv> <out dispositions.tsv>")
    src, dst = argv[1], argv[2]
    rows = []
    with open(src, newline="") as f:
        r = csv.DictReader(f, delimiter="\t")
        for row in r:
            if "minimal" not in row["closure"].split("+"):
                continue
            rows.append(row)
    names = {row["name"] for row in rows}
    for n in RUNTIME_LIBPTHREAD:
        if n not in names:
            sys.exit(f"gen-dispositions: runtime name {n} missing from {src}")
    for n in OVERRIDES:
        if n not in names:
            sys.exit(f"gen-dispositions: override {n} not in the minimal closure")
    out = []
    for row in sorted(rows, key=lambda r: r["name"]):
        name = row["name"]
        disp = OVERRIDES.get(name, row["disposition"])
        base = disp.split()[0]
        if base not in ("identical", "translate", "implement"):
            sys.exit(f"gen-dispositions: {name}: unknown disposition {disp!r}")
        if base == "identical":
            if name in RENAMES or name in DATA:
                sys.exit(f"gen-dispositions: {name} is 'identical' but listed as rename/data")
            target = name
        elif name in RENAMES:
            target = RENAMES[name]
        elif name in DATA:
            target = name
        else:
            target = "nvg_" + name
        runtime = "libpthread.so.0" if name in RUNTIME_LIBPTHREAD else "-"
        out.append((name, disp, target, runtime))
    with open(dst, "w") as f:
        f.write("name\tdisposition\ttarget\truntime\n")
        for t in out:
            f.write("\t".join(t) + "\n")
    print(f"gen-dispositions: {len(out)} names -> {dst}")


if __name__ == "__main__":
    main(sys.argv)
