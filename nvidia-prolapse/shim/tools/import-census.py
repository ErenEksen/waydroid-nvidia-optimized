#!/usr/bin/env python3
"""Import census of the NVIDIA ICD closure (run with `python3 -I`).

For every blob given on the command line, read the dynamic symbol table,
.gnu.version and .gnu.version_r directly from the ELF file and collect each
undefined symbol whose versym index names a verneed entry. The verneed *file*
(e.g. libc.so.6) is the front that must export the name: the bionic linker
requires that file to be the DT_SONAME of a direct DT_NEEDED child
(linker.cpp VersionTracker::init_verneed) and binds the reference only to an
unversioned definition in a DSO without that version (find_verdef_version_index,
check_symbol_version).

--reference FILE (repeatable) adds the names of an earlier census output, e.g. the
per-driver reference lists in imports/ (DESIGN.md "Driver branches"): the result is the
union over every given blob and reference, which is what a shim build for several driver
versions exports. A reference row is checked like a blob import.

Then add the names marked runtime=<front> in dispositions.tsv, and fail on:
  - an imported glibc name with no row in dispositions.tsv  (G0 invariant)
  - a verneed file that is not one of the six fronts
  - a dispositions.tsv row that is neither imported nor a runtime name
    (primary table, x86_64 only; with --arch x86 a base row the i386 closure
    does not import is reported, not an error, but every row of the per-arch
    override table must be imported or a runtime name)

--arch x86 reads ELF32 i386 blobs, uses the i386 front set (ld-linux.so.2 instead
of ld-linux-x86-64.so.2) and applies --override (dispositions-x86.tsv) on top of
--dispositions, row by row (DESIGN.md "32-bit x86").

--allow-unused reports table rows the inputs do not import instead of failing (used to
cut one driver's reference list, imports/<version>-<arch>.tsv, from its libraries).

--covered-by CENSUS checks the blobs against the export set of a finished shim build (its
gen/census.tsv): every imported (front, name) must be in it, else the shim build does not
cover this driver and the check fails naming each missing import.

Output (--out): TSV  front  name  versions  blobs
"""
import argparse
import struct
import sys

FRONTS_BY_ARCH = {
    "x86_64": ["libc.so.6", "libm.so.6", "libpthread.so.0", "librt.so.1",
               "libdl.so.2", "ld-linux-x86-64.so.2"],
    "x86": ["libc.so.6", "libm.so.6", "libpthread.so.0", "librt.so.1",
            "libdl.so.2", "ld-linux.so.2"],
}
# (ELF class byte, e_machine, section header struct, symbol struct)
ELF_BY_ARCH = {
    "x86_64": (2, 62, "<IIQQQQIIQQ", "<IBBHQQ"),
    "x86": (1, 3, "<IIIIIIIIII", "<IIIBBH"),
}

SHT_DYNSYM = 11
SHT_GNU_VERSYM = 0x6fffffff
SHT_GNU_VERNEED = 0x6ffffffe


class Elf:
    def __init__(self, path, arch):
        with open(path, "rb") as f:
            self.d = f.read()
        d = self.d
        cls, machine, shfmt, self.symfmt = ELF_BY_ARCH[arch]
        if d[:4] != b"\x7fELF" or d[4] != cls or d[5] != 1:
            raise SystemExit(f"import-census: {path}: not an ELF{64 if cls == 2 else 32} "
                             f"little-endian file (--arch {arch})")
        (e_machine,) = struct.unpack_from("<H", d, 0x12)
        if e_machine != machine:
            raise SystemExit(f"import-census: {path}: e_machine {e_machine}, expected {machine} "
                             f"(--arch {arch})")
        if cls == 2:
            (self.e_shoff,) = struct.unpack_from("<Q", d, 0x28)
            self.e_shentsize, self.e_shnum = struct.unpack_from("<HH", d, 0x3A)
        else:
            (self.e_shoff,) = struct.unpack_from("<I", d, 0x20)
            self.e_shentsize, self.e_shnum = struct.unpack_from("<HH", d, 0x2E)
        self.sh = []
        for i in range(self.e_shnum):
            o = self.e_shoff + i * self.e_shentsize
            name, typ, flags, addr, off, size, link, info, align, entsize = \
                struct.unpack_from(shfmt, d, o)
            self.sh.append(dict(type=typ, off=off, size=size, link=link,
                                info=info, entsize=entsize))

    def section(self, typ):
        s = [x for x in self.sh if x["type"] == typ]
        if len(s) > 1:
            raise SystemExit("import-census: more than one section of type %#x" % typ)
        return s[0] if s else None

    def cstr(self, off):
        e = self.d.index(b"\0", off)
        return self.d[off:e].decode()


def census(path, arch):
    e = Elf(path, arch)
    dynsym = e.section(SHT_DYNSYM)
    if dynsym is None:
        raise SystemExit(f"import-census: {path}: no .dynsym")
    strtab = e.sh[dynsym["link"]]["off"]
    versym = e.section(SHT_GNU_VERSYM)
    verneed = e.section(SHT_GNU_VERNEED)
    if versym is None or verneed is None:
        raise SystemExit(f"import-census: {path}: no .gnu.version/.gnu.version_r")
    vstr = e.sh[verneed["link"]]["off"]
    # versym index -> (file, version)
    idx = {}
    off = verneed["off"]
    for _ in range(verneed["info"]):
        vn_version, vn_cnt, vn_file, vn_aux, vn_next = struct.unpack_from("<HHIII", e.d, off)
        if vn_version != 1:
            raise SystemExit(f"import-census: {path}: verneed version {vn_version}")
        fname = e.cstr(vstr + vn_file)
        a = off + vn_aux
        for _ in range(vn_cnt):
            vna_hash, vna_flags, vna_other, vna_name, vna_next = struct.unpack_from("<IHHII", e.d, a)
            idx[vna_other] = (fname, e.cstr(vstr + vna_name))
            a += vna_next
        off += vn_next
    n = dynsym["size"] // dynsym["entsize"]
    out = []
    for i in range(1, n):
        o = dynsym["off"] + i * dynsym["entsize"]
        if arch == "x86_64":
            st_name, st_info, st_other, st_shndx, st_value, st_size = struct.unpack_from(e.symfmt, e.d, o)
        else:
            st_name, st_value, st_size, st_info, st_other, st_shndx = struct.unpack_from(e.symfmt, e.d, o)
        if st_shndx != 0:
            continue
        (vs,) = struct.unpack_from("<H", e.d, versym["off"] + 2 * i)
        vs &= 0x7fff
        name = e.cstr(strtab + st_name)
        if vs in (0, 1):
            continue  # unversioned reference: not a glibc import (blob-to-blob, weak hooks)
        if vs not in idx:
            raise SystemExit(f"import-census: {path}: {name}: versym {vs} has no verneed entry")
        out.append((name, idx[vs][0], idx[vs][1]))
    return out


def read_dispositions(path):
    disp = {}
    with open(path) as f:
        hdr = f.readline().rstrip("\n").split("\t")
        if hdr != ["name", "disposition", "target", "runtime"]:
            raise SystemExit(f"import-census: {path}: bad header {hdr}")
        for line in f:
            name, d, target, runtime = line.rstrip("\n").split("\t")
            disp[name] = (d, target, runtime)
    return disp


def read_census(path):
    """Rows (front, name, versions, blobs) of a census output (--out of an earlier run)."""
    with open(path) as f:
        hdr = f.readline().rstrip("\n").split("\t")
        if hdr != ["front", "name", "versions", "blobs"]:
            raise SystemExit(f"import-census: {path}: bad header {hdr}")
        return [tuple(line.rstrip("\n").split("\t")) for line in f if line.strip()]


def covered_by(path, sets, arch):
    """Fail unless every imported (front, name) of the blobs is exported by the build behind `path`."""
    have = {(fr, name) for fr, name, _, _ in read_census(path)}
    missing = sorted((fr, name, ",".join(sorted(b))) for fr, names in sets.items()
                     for name, (_, b) in names.items() if (fr, name) not in have and "runtime-dlvsym" not in b)
    for fr, name, blobs in missing:
        print(f"import-census: {name} ({fr}) imported by {blobs} is not exported by the shim build "
              f"{path}", file=sys.stderr)
    if missing:
        sys.exit(f"import-census: --arch {arch}: {len(missing)} imports of this driver are not covered by the "
                 f"shim build; it was cut for other driver versions (update the shims for this driver)")
    print(f"import-census: --arch {arch}: every import of these blobs is exported by {path}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dispositions", required=True)
    ap.add_argument("--override", help="per-arch rows applied on top of --dispositions")
    ap.add_argument("--arch", default="x86_64", choices=sorted(FRONTS_BY_ARCH))
    ap.add_argument("--out")
    ap.add_argument("--reference", action="append", default=[],
                    help="an earlier census output whose names are added (repeatable)")
    ap.add_argument("--allow-unused", action="store_true",
                    help="report table rows the inputs do not import instead of failing")
    ap.add_argument("--covered-by", metavar="CENSUS",
                    help="check that a finished shim build (its census.tsv) exports every import of the blobs")
    ap.add_argument("blobs", nargs="*")
    a = ap.parse_args()
    if not a.blobs and not a.reference:
        ap.error("give blobs and/or --reference lists")
    if a.covered_by and (a.reference or not a.blobs):
        ap.error("--covered-by checks blobs only (no --reference)")
    if not a.covered_by and not a.out:
        ap.error("--out is required")
    FRONTS = FRONTS_BY_ARCH[a.arch]

    disp = read_dispositions(a.dispositions)
    override = read_dispositions(a.override) if a.override else {}
    disp.update(override)

    sets = {fr: {} for fr in FRONTS}  # front -> name -> (versions set, blobs set)
    errors = []
    for blob in a.blobs:
        base = blob.rsplit("/", 1)[-1]
        for name, fname, ver in census(blob, a.arch):
            if fname not in sets:
                errors.append(f"import-census: {base}: {name}@{ver} from {fname}, which is not a front")
                continue
            if name not in disp:
                errors.append(f"import-census: undispositioned import {name}@{ver} ({fname}) in {base}")
                continue
            v, b = sets[fname].setdefault(name, (set(), set()))
            v.add(ver)
            b.add(base)
    for ref in a.reference:
        for fname, name, vers, blobs in read_census(ref):
            if "runtime-dlvsym" in blobs.split(","):
                continue  # added below from the table, as for blobs
            if fname not in sets:
                errors.append(f"import-census: {ref}: {name} from {fname}, which is not a front")
                continue
            if name not in disp:
                errors.append(f"import-census: undispositioned import {name}@{vers} ({fname}) in {ref}")
                continue
            v, b = sets[fname].setdefault(name, (set(), set()))
            v.update(x for x in vers.split(",") if x != "-")
            b.update(blobs.split(","))
    if a.covered_by:
        if errors:
            for e in errors:
                print(e, file=sys.stderr)
            sys.exit(1)
        covered_by(a.covered_by, sets, a.arch)
        return
    imported = {n for s in sets.values() for n in s}
    unused = []
    for name, (d, target, runtime) in disp.items():
        if runtime != "-":
            if runtime not in sets:
                errors.append(f"import-census: {name}: runtime front {runtime} unknown")
                continue
            v, b = sets[runtime].setdefault(name, (set(), set()))
            b.add("runtime-dlvsym")
        elif name not in imported:
            if a.allow_unused:
                unused.append(name)
            elif a.override is None or name in override:
                errors.append(f"import-census: dispositions row {name} is neither imported "
                              f"by the given blobs nor a runtime name (stale table?)")
            else:
                unused.append(name)
    if errors:
        for e in errors:
            print(e, file=sys.stderr)
        sys.exit(1)
    with open(a.out, "w") as f:
        f.write("front\tname\tversions\tblobs\n")
        for fr in FRONTS:
            for name in sorted(sets[fr]):
                v, b = sets[fr][name]
                f.write(f"{fr}\t{name}\t{','.join(sorted(v)) or '-'}\t{','.join(sorted(b))}\n")
    total = len({n for s in sets.values() for n in s})
    print("import-census: " + ", ".join(f"{fr} {len(sets[fr])}" for fr in FRONTS)
          + f"; {total} distinct names, all dispositioned")
    if unused:
        print(f"import-census: --arch {a.arch}: {len(unused)} table rows not imported by these "
              f"inputs (no export generated): {' '.join(sorted(unused))}")


if __name__ == "__main__":
    main()
