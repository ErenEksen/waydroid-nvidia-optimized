#!/usr/bin/env python3
"""Artifact checks for the built shims (run with `python3 -I`). Exits non-zero
on any violation. Reads the real ELF files with llvm-readelf.

Fronts (out/<SONAME>):
  - DT_SONAME is exactly <SONAME>; DT_NEEDED is exactly [libnvglibc.so]
  - every expected name (gen/<SONAME>.names) is defined GLOBAL DEFAULT and
    unversioned; FUNC for trampolines, OBJECT of size 8 for data
  - the defined dynamic symbols are exactly those names plus the eleven
    nvg_v_<V>@@<V> dummy version symbols
  - nothing is undefined except nvg_* symbols that the core exports
  - .gnu.version_d defines exactly the eleven bionic version names; no verneed
  - one .init_array entry; no TEXTREL
Core (out/libnvglibc.so):
  - DT_SONAME libnvglibc.so; no .gnu.version_d (no version script)
  - defined dynamic symbols are exactly gen/core.exports (all nvg_*)
  - every version it needs from libc.so/libm.so/libdl.so is one the fronts
    define as a dummy (the load-order-independence premise of DESIGN.md)
  - no unversioned undefined reference names a front export
"""
import argparse
import os
import re
import subprocess
import sys

BIONIC_VERSIONS = ["LIBC", "LIBC_N", "LIBC_O", "LIBC_P", "LIBC_Q", "LIBC_R", "LIBC_S",
                   "LIBC_T", "LIBC_DEPRECATED", "LIBC_PLATFORM", "LIBC_PRIVATE"]
CORE = "libnvglibc.so"

errors = []


def err(msg):
    errors.append(msg)


def readelf(tool, args, path):
    return subprocess.run([tool] + args + [path], check=True, capture_output=True,
                          text=True).stdout


def dynamic(tool, path):
    out = readelf(tool, ["-d"], path)
    needed = re.findall(r"\(NEEDED\)\s+Shared library: \[(.*)\]", out)
    soname = re.findall(r"\(SONAME\)\s+Library soname: \[(.*)\]", out)
    tags = re.findall(r"^\s*0x[0-9a-f]+ \((\w+)\)\s+(.*)$", out, re.M)
    return needed, soname, dict((t, v) for t, v in tags)


def dynsyms(tool, path):
    out = readelf(tool, ["--dyn-syms", "-W"], path)
    rows = []
    for line in out.splitlines():
        m = re.match(r"^\s*(\d+): ([0-9a-f]+)\s+(\d+)\s+(\w+)\s+(\w+)\s+(\w+)\s+(\w+)\s*(\S*)$",
                     line)
        if not m or m.group(1) == "0":
            continue
        num, val, size, typ, bind, vis, ndx, name = m.groups()
        base, _, ver = name.partition("@")
        rows.append(dict(name=name, base=base, ver=ver.lstrip("@"), versioned=bool(ver),
                         size=int(size), type=typ, bind=bind, vis=vis, undef=(ndx == "UND")))
    return rows


def versions(tool, path):
    out = readelf(tool, ["-V"], path)
    verdef = []
    verneed = {}
    sec = None
    cur = None
    for line in out.splitlines():
        if line.startswith("Version definition section"):
            sec = "d"
        elif line.startswith("Version needs section"):
            sec = "r"
        elif line.startswith("Version symbols section"):
            sec = "s"
        elif sec == "d":
            m = re.search(r"Flags: (\w+)\s+Index: \d+\s+Cnt: \d+\s+Name: (\S+)", line)
            if m and m.group(1) != "BASE":
                verdef.append(m.group(2))
        elif sec == "r":
            m = re.search(r"File: (\S+)", line)
            if m:
                cur = m.group(1)
                verneed[cur] = []
                continue
            m = re.search(r"Name: (\S+)", line)
            if m and cur:
                verneed[cur].append(m.group(1))
    return verdef, verneed


def read_lines(path):
    with open(path) as f:
        return [l.rstrip("\n") for l in f if l.strip()]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--readelf", required=True)
    ap.add_argument("--gen", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--ptr-size", type=int, default=8, choices=(4, 8),
                    help="pointer size of the target: data objects and init_array entries")
    ap.add_argument("fronts", nargs="+")
    a = ap.parse_args()
    R = a.readelf

    # ---- core ----
    core_path = os.path.join(a.out, CORE)
    core_expected = set(read_lines(os.path.join(a.gen, "core.exports")))
    needed, soname, tags = dynamic(R, core_path)
    if soname != [CORE]:
        err(f"{CORE}: SONAME {soname}, expected [{CORE}]")
    syms = dynsyms(R, core_path)
    core_defined = {s["base"] for s in syms if not s["undef"]}
    for s in syms:
        if not s["undef"] and not s["base"].startswith("nvg_"):
            err(f"{CORE}: defines {s['name']} (only nvg_* may be defined)")
        if not s["undef"] and s["versioned"]:
            err(f"{CORE}: export {s['name']} is versioned (no version script allowed)")
    for n in sorted(core_expected - core_defined):
        err(f"{CORE}: missing export {n}")
    for n in sorted(core_defined - core_expected):
        err(f"{CORE}: unexpected export {n}")
    cverdef, cverneed = versions(R, core_path)
    if cverdef:
        err(f"{CORE}: has version definitions {cverdef} (no version script allowed)")
    for lib in ("libc.so", "libm.so", "libdl.so"):
        for v in cverneed.get(lib, []):
            if v not in BIONIC_VERSIONS:
                err(f"{CORE}: needs {lib}@{v}, which the fronts do not define as a dummy "
                    f"version: a front could satisfy that reference (add {v} to the list)")
    print(f"check-artifacts: {CORE}: {len(core_defined)} exports, all nvg_*; "
          f"NEEDED {needed}; verneed " +
          ", ".join(f"{k}:{'/'.join(v)}" for k, v in sorted(cverneed.items())))

    # An unversioned reference matches any non-hidden definition
    # (check_symbol_version), so none may name something a front exports.
    all_front_names = set()
    for fr in a.fronts:
        for line in read_lines(os.path.join(a.gen, fr + ".names")):
            all_front_names.add(line.split("\t")[0])
    for s in syms:
        if s["undef"] and not s["versioned"] and s["base"] in all_front_names:
            err(f"{CORE}: unversioned reference to {s['base']}, which a front exports: it "
                f"could bind to the front")

    # ---- fronts ----
    total = 0
    for fr in a.fronts:
        path = os.path.join(a.out, fr)
        expected = {}
        for line in read_lines(os.path.join(a.gen, fr + ".names")):
            name, kind = line.split("\t")
            expected[name] = kind
        needed, soname, tags = dynamic(R, path)
        if soname != [fr]:
            err(f"{fr}: SONAME {soname}, expected [{fr}]")
        if needed != [CORE]:
            err(f"{fr}: NEEDED {needed}, expected exactly [{CORE}]")
        if "TEXTREL" in tags or "TEXTREL" in tags.get("FLAGS", ""):
            err(f"{fr}: has TEXTREL")
        if tags.get("INIT_ARRAYSZ", "").split()[0:1] != [str(a.ptr_size)]:
            err(f"{fr}: INIT_ARRAYSZ {tags.get('INIT_ARRAYSZ')!r}, expected one entry "
                f"({a.ptr_size} bytes)")
        syms = dynsyms(R, path)
        defined = {}
        for s in syms:
            if s["undef"]:
                if not s["base"].startswith("nvg_"):
                    err(f"{fr}: undefined non-nvg symbol {s['name']}")
                elif s["base"] not in core_defined:
                    err(f"{fr}: undefined {s['name']} is not exported by {CORE}")
                if s["versioned"]:
                    err(f"{fr}: versioned undefined reference {s['name']}")
                continue
            defined[s["base"]] = s
        dummies = {f"nvg_v_{v}": v for v in BIONIC_VERSIONS}
        n_ok = n_data = 0
        for name, kind in expected.items():
            s = defined.get(name)
            if s is None:
                err(f"{fr}: expected export {name} is not defined")
                continue
            if s["bind"] != "GLOBAL" or s["vis"] != "DEFAULT":
                err(f"{fr}: {name} is {s['bind']} {s['vis']}, expected GLOBAL DEFAULT")
            if s["versioned"]:
                err(f"{fr}: {name} is versioned ({s['name']}), expected unversioned")
            if kind == "DATA":
                if s["type"] != "OBJECT" or s["size"] != a.ptr_size:
                    err(f"{fr}: {name} is {s['type']} size {s['size']}, expected OBJECT size "
                        f"{a.ptr_size}")
                n_data += 1
            elif s["type"] != "FUNC":
                err(f"{fr}: {name} is {s['type']}, expected FUNC")
            n_ok += 1
        for name, s in defined.items():
            if name in expected:
                continue
            if name in dummies:
                if s["ver"] != dummies[name]:
                    err(f"{fr}: {name} has version {s['ver']!r}, expected {dummies[name]}")
                continue
            err(f"{fr}: unexpected export {s['name']}")
        for d in dummies:
            if d not in defined:
                err(f"{fr}: dummy version symbol {d} missing")
        verdef, verneed = versions(R, path)
        if sorted(verdef) != sorted(BIONIC_VERSIONS):
            err(f"{fr}: .gnu.version_d defines {verdef}, expected {BIONIC_VERSIONS}")
        if verneed:
            err(f"{fr}: has version needs {verneed}")
        unds = sorted({s["base"] for s in syms if s["undef"]})
        total += n_ok
        print(f"check-artifacts: {fr}: {n_ok}/{len(expected)} names exported GLOBAL DEFAULT "
              f"unversioned ({n_data} data OBJECT size {a.ptr_size}); {len(verdef)} LIBC* verdefs; "
              f"NEEDED {needed}; UND {len(unds)} (all nvg_*)")

    if errors:
        for e in errors:
            print("check-artifacts: FAIL: " + e, file=sys.stderr)
        sys.exit(1)
    print(f"check-artifacts: OK: {total} glibc names over {len(a.fronts)} fronts")


if __name__ == "__main__":
    main()
