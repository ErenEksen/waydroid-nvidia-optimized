#!/usr/bin/env python3
"""Generate gen/constants.h: glibc<->bionic constant tables (run with `python3 -I`).

Values are taken from both C libraries' headers *by the compilers*, never typed
in and never by running code: a probe source defines
    const long long nvg_c_<NAME> = (long long)(<NAME>);
for each constant, is compiled to assembly once with the host gcc (glibc
headers, -D_GNU_SOURCE) and once with the NDK clang (bionic headers), and the
initialisers are read back from the `.quad` directives. (Preprocessing alone is
not enough: glibc's _SC_* and nl_item names are enumerators, `#define`d to
themselves, so `gcc -E` leaves them symbolic.) A name a header does not declare
is detected from the compiler's "undeclared" diagnostic and dropped for that
side, then the probe is recompiled.

Families:
  map      _SC_*     (names from bionic bits/sysconf.h)  -> nvg_sc_map[]
  map      nl_item   (names from bionic langinfo.h)      -> nvg_nl_item_map[]
  identical LC_*, LC_*_MASK, RTLD_*, SA_*, SIG_{BLOCK,UNBLOCK,SETMASK},
            PTHREAD_MUTEX_*, PTHREAD_PRIO_*, CLOCK_*
An "identical" family that differs is a build error if any name consuming it
goes through a plain or renamed trampoline (dispositions.tsv target not nvg_*).
LC_* / LC_*_MASK differing is always a build error (the __newlocale family would
need a translator; see DESIGN.md).
"""
import argparse
import os
import re
import subprocess
import sys

HEADERS = ["stdint.h", "unistd.h", "langinfo.h", "locale.h", "dlfcn.h", "signal.h",
           "pthread.h", "time.h"]

IDENTICAL = {
    "LC": ["LC_CTYPE", "LC_NUMERIC", "LC_TIME", "LC_COLLATE", "LC_MONETARY",
           "LC_MESSAGES", "LC_ALL", "LC_PAPER", "LC_NAME", "LC_ADDRESS",
           "LC_TELEPHONE", "LC_MEASUREMENT", "LC_IDENTIFICATION"],
    "LC_MASK": ["LC_CTYPE_MASK", "LC_NUMERIC_MASK", "LC_TIME_MASK", "LC_COLLATE_MASK",
                "LC_MONETARY_MASK", "LC_MESSAGES_MASK", "LC_PAPER_MASK", "LC_NAME_MASK",
                "LC_ADDRESS_MASK", "LC_TELEPHONE_MASK", "LC_MEASUREMENT_MASK",
                "LC_IDENTIFICATION_MASK", "LC_ALL_MASK", "LC_GLOBAL_LOCALE"],
    "RTLD": ["RTLD_LAZY", "RTLD_NOW", "RTLD_GLOBAL", "RTLD_LOCAL", "RTLD_NOLOAD",
             "RTLD_NODELETE", "RTLD_DEFAULT", "RTLD_NEXT", "RTLD_DEEPBIND"],
    "SA": ["SA_NOCLDSTOP", "SA_NOCLDWAIT", "SA_SIGINFO", "SA_ONSTACK", "SA_RESTART",
           "SA_NODEFER", "SA_RESETHAND", "SA_INTERRUPT", "SA_NOMASK", "SA_ONESHOT",
           "SA_STACK", "SA_RESTORER"],
    "SIG_HOW": ["SIG_BLOCK", "SIG_UNBLOCK", "SIG_SETMASK"],
    "PTHREAD_MUTEX": ["PTHREAD_MUTEX_NORMAL", "PTHREAD_MUTEX_RECURSIVE",
                      "PTHREAD_MUTEX_ERRORCHECK", "PTHREAD_MUTEX_DEFAULT"],
    "PTHREAD_PRIO": ["PTHREAD_PRIO_NONE", "PTHREAD_PRIO_INHERIT", "PTHREAD_PRIO_PROTECT"],
    "PTHREAD_SCHED": ["PTHREAD_INHERIT_SCHED", "PTHREAD_EXPLICIT_SCHED"],
    "PTHREAD_CREATE": ["PTHREAD_CREATE_JOINABLE", "PTHREAD_CREATE_DETACHED"],
    "SCHED": ["SCHED_OTHER", "SCHED_FIFO", "SCHED_RR", "SCHED_BATCH", "SCHED_IDLE"],
    "CLOCK": ["CLOCK_REALTIME", "CLOCK_MONOTONIC", "CLOCK_PROCESS_CPUTIME_ID",
              "CLOCK_THREAD_CPUTIME_ID", "CLOCK_MONOTONIC_RAW", "CLOCK_REALTIME_COARSE",
              "CLOCK_MONOTONIC_COARSE", "CLOCK_BOOTTIME", "CLOCK_REALTIME_ALARM",
              "CLOCK_BOOTTIME_ALARM", "CLOCK_TAI"],
}

# glibc names whose arguments carry constants of a family (DESIGN.md
# "Generated constant tables").
CONSUMERS = {
    "SC": ["sysconf"],
    "NL": ["__nl_langinfo_l", "nl_langinfo", "nl_langinfo_l"],
    "LC": ["setlocale", "__newlocale", "newlocale"],
    "LC_MASK": ["__newlocale", "newlocale", "__uselocale", "uselocale",
                "__duplocale", "duplocale", "__freelocale", "freelocale"],
    "RTLD": ["dlopen", "dlsym", "dlvsym"],
    "SA": ["sigaction"],
    "SIG_HOW": ["sigprocmask", "pthread_sigmask"],
    "PTHREAD_MUTEX": ["pthread_mutexattr_settype"],
    "PTHREAD_PRIO": ["pthread_mutexattr_setprotocol"],
    "PTHREAD_SCHED": ["pthread_attr_setinheritsched"],
    "PTHREAD_CREATE": ["pthread_attr_setdetachstate"],
    "SCHED": ["pthread_attr_setschedpolicy"],
    "CLOCK": ["pthread_condattr_setclock", "clock_gettime", "clock_getres",
              "clock_nanosleep", "timer_create", "clock_settime"],
}


def compile_probe(cc, names, workdir, tag):
    names = list(names)
    dropped = []
    src = os.path.join(workdir, f"probe-{tag}.c")
    for _ in range(50):
        with open(src, "w") as f:
            for h in HEADERS:
                f.write(f"#include <{h}>\n")
            for n in names:
                # through uintptr_t: pointer-valued constants (RTLD_DEFAULT, LC_GLOBAL_LOCALE)
                # widen the same way in gcc and clang (gcc sign-extends a 32-bit pointer)
                f.write(f"const long long nvg_c_{n} = (long long)(uintptr_t)({n});\n")
        p = subprocess.run(cc + ["-S", "-O0", "-o", "-", src], capture_output=True,
                           text=True, env=dict(os.environ, LC_ALL="C", LANG="C"))
        if p.returncode == 0:
            return parse_asm(p.stdout, names), dropped
        bad = set()
        for m in re.finditer(r"undeclared identifier '(\w+)'|'(\w+)' undeclared", p.stderr):
            bad.add(m.group(1) or m.group(2))
        bad &= set(names)
        if not bad:
            sys.stderr.write(p.stderr)
            raise SystemExit(f"gen-constants: {tag} probe failed to compile for a reason "
                             "other than an undeclared name")
        names = [n for n in names if n not in bad]
        dropped += sorted(bad)
    raise SystemExit(f"gen-constants: {tag} probe did not converge")


def parse_asm(asm, names):
    vals = {}
    cur = None
    lo = None  # i386 gcc spells a 64-bit initialiser as two .long (low word first)
    for line in asm.splitlines():
        s = line.split("#", 1)[0].strip()
        m = re.match(r"^nvg_c_(\w+):$", s)
        if m:
            cur = m.group(1)
            lo = None
            continue
        if cur is None:
            continue
        m = re.match(r"^\.(long|4byte)\s+(-?\w+)$", s)
        if m:
            w = int(m.group(2), 0) & 0xffffffff
            if lo is None:
                lo = w
            else:
                v = lo | (w << 32)
                vals[cur] = v - (1 << 64) if v >> 63 else v
                cur = None
            continue
        m = re.match(r"^\.(quad|8byte)\s+(-?\w+)$", s)
        if m:
            vals[cur] = int(m.group(2), 0)
            cur = None
            continue
        m = re.match(r"^\.zero\s+8$", s)
        if m:
            vals[cur] = 0
            cur = None
            continue
    missing = [n for n in names if n not in vals]
    if missing:
        raise SystemExit(f"gen-constants: no value parsed for {missing[:5]}")
    return vals


def glibc_aliases(cc, names, workdir):
    """Names that glibc's preprocessor expands to a *different* identifier."""
    src = os.path.join(workdir, "probe-glibc-alias.c")
    with open(src, "w") as f:
        f.write("#include <unistd.h>\n#include <langinfo.h>\n")
        for n in names:
            f.write(f"nvg_alias {n} = {n}\n")
    p = subprocess.run(cc + ["-E", "-P", src], capture_output=True, text=True)
    if p.returncode != 0:
        sys.stderr.write(p.stderr)
        raise SystemExit("gen-constants: glibc alias probe failed")
    out = {}
    # Both operands are expanded, so pair output lines with the probe order.
    exp = [l.strip() for l in p.stdout.splitlines() if l.startswith("nvg_alias ")]
    for n, line in zip(names, exp):
        tok = line.split(" = ", 1)[1].strip()
        if re.fullmatch(r"\w+", tok) and tok != n and tok in names:
            out[n] = tok
    return out


def header_names(path, rx):
    with open(path) as f:
        return sorted(set(re.findall(rx, f.read(), re.M)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host-cc", required=True)
    ap.add_argument("--ndk-cc", required=True)
    ap.add_argument("--ndk-include", required=True, help="NDK sysroot usr/include")
    ap.add_argument("--dispositions", required=True)
    ap.add_argument("--override", help="per-arch rows applied on top of --dispositions")
    ap.add_argument("--out", required=True)
    ap.add_argument("--workdir", required=True)
    a = ap.parse_args()

    disp = {}
    for path in [a.dispositions] + ([a.override] if a.override else []):
        with open(path) as f:
            f.readline()
            for line in f:
                name, d, target, runtime = line.rstrip("\n").split("\t")
                disp[name] = target

    sc_names = header_names(os.path.join(a.ndk_include, "bits/sysconf.h"),
                            r"^#define\s+(_SC_\w+)\s")
    nl_names = [n for n in header_names(os.path.join(a.ndk_include, "langinfo.h"),
                                        r"^#define\s+([A-Z][A-Z0-9_]*)\s+\d+\s*$")]
    if not sc_names or not nl_names:
        raise SystemExit("gen-constants: no _SC_* / nl_item names found in the NDK headers")
    all_names = sc_names + nl_names + [n for v in IDENTICAL.values() for n in v]
    all_names = list(dict.fromkeys(all_names))

    host_cc = a.host_cc.split() + ["-D_GNU_SOURCE"]
    ndk_cc = a.ndk_cc.split()
    g, g_drop = compile_probe(host_cc, all_names, a.workdir, "glibc")
    b, b_drop = compile_probe(ndk_cc, all_names, a.workdir, "bionic")

    report = []
    errors = []

    galias = glibc_aliases(host_cc, sc_names + nl_names, a.workdir)

    def mapping(fam, names):
        rows = {}
        only_b, only_g, same = [], [], 0
        for n in names:
            if n in g and n in b:
                same += g[n] == b[n]
                if n in galias:
                    # glibc spells n as another name (e.g. _SC_PAGE_SIZE ->
                    # _SC_PAGESIZE): the canonical name's bionic id is used.
                    report.append(f"{fam}: {n} is a glibc alias of {galias[n]}; "
                                  f"mapped via {galias[n]}")
                    continue
                if g[n] in rows and rows[g[n]][0] != b[n]:
                    errors.append(f"{fam}: glibc value {g[n]} maps to two bionic values "
                                  f"({rows[g[n]][1]}={rows[g[n]][0]}, {n}={b[n]})")
                rows.setdefault(g[n], (b[n], n))
            elif n in b:
                only_b.append(n)
            else:
                only_g.append(n)
        report.append(f"{fam}: {len(rows)} glibc ids mapped ({same} numerically equal); "
                      f"bionic-only (no glibc name): {' '.join(only_b) or '-'}")
        return sorted(rows.items())

    sc_map = mapping("_SC_*", sc_names)
    nl_map = mapping("nl_item", nl_names)

    differing = {}
    for fam, names in IDENTICAL.items():
        both = [n for n in names if n in g and n in b]
        diff = [n for n in both if g[n] != b[n]]
        only_g = [n for n in names if n in g and n not in b]
        only_b = [n for n in names if n in b and n not in g]
        neither = [n for n in names if n not in g and n not in b]
        state = "IDENTICAL" if not diff else "DIFFER: " + ", ".join(
            f"{n} glibc={g[n]:#x} bionic={b[n]:#x}" for n in diff)
        extra = []
        if only_g:
            extra.append("glibc-only " + " ".join(only_g))
        if only_b:
            extra.append("bionic-only " + " ".join(only_b))
        if neither:
            extra.append("in neither " + " ".join(neither))
        report.append(f"{fam}: {len(both)} compared, {state}"
                      + (f" ({'; '.join(extra)})" if extra else ""))
        if diff:
            differing[fam] = diff

    for fam in ("LC", "LC_MASK"):
        if fam in differing:
            errors.append(f"{fam} differs between glibc and bionic: the __newlocale family "
                          f"needs a translator (DESIGN.md) - not implemented, stop and decide")
    for fam in list(differing) + ["SC", "NL"]:
        for c in CONSUMERS.get(fam, []):
            t = disp.get(c)
            if t is not None and not t.startswith("nvg_"):
                errors.append(f"{fam} constants differ but {c} is a trampoline to bionic "
                              f"{t} (needs a translator)")

    with open(a.out, "w") as f:
        f.write("/* Generated by tools/gen-constants.py from the glibc and bionic headers. */\n")
        f.write("#pragma once\n\nstruct nvg_cmap { int glibc; int bionic; const char *name; };\n\n")
        for cname, rows in (("nvg_sc_map", sc_map), ("nvg_nl_item_map", nl_map)):
            f.write(f"static const struct nvg_cmap {cname}[] = {{\n")
            for gv, (bv, n) in rows:
                f.write(f"    {{ {gv}, {bv}, \"{n}\" }},\n")
            f.write("};\n")
            f.write(f"#define {cname.upper()}_N (sizeof({cname}) / sizeof({cname}[0]))\n\n")
        for n in ("SA_RESTORER",):
            if n in b:
                f.write(f"#define NVG_BIONIC_{n} {b[n]:#x}\n")
    print("gen-constants report:")
    for r in report:
        print("  " + r)
    if errors:
        for e in errors:
            print("gen-constants: ERROR: " + e, file=sys.stderr)
        sys.exit(1)
    print(f"gen-constants: wrote {a.out}")


if __name__ == "__main__":
    main()
