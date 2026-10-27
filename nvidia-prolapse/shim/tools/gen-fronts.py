#!/usr/bin/env python3
"""Generate the six front libraries' sources (run with `python3 -I`).

Inputs: gen/census.tsv (import-census.py), dispositions.tsv, the NDK API-33
libc/libm/libdl stubs (read with llvm-nm). For each front SONAME writes
  gen/<SONAME>.S        trampolines, slots, data objects, dummy versions, init
  gen/<SONAME>.map      version script: one dummy symbol per bionic version name
  gen/<SONAME>_table.c  struct nvg_front handed to nvg_front_init()
  gen/<SONAME>.names    expected exports: name<TAB>kind (for check-artifacts.py)
and gen/core.exports (every nvg_* symbol the core must export).

Mechanism per name (DESIGN.md "Trampolines"):
  target nvg_*              -> `jmp nvg_<name>@PLT`                 (kind CORE)
  target is a bionic object -> exported 8-byte object, filled at init (kind DATA)
  target is a bionic func   -> `jmp *slot(%rip)`, slot filled by dlsym (kind SLOT)
A bionic target missing from the stubs is a build error.

--arch x86 (i686, DESIGN.md "32-bit x86"): the i386 front set (ld-linux.so.2), 4-byte
slots/data/init_array entries, and trampolines that never rely on the caller's %ebx: a
caller may be non-PIC (libnvidia-eglcore i386 calls imports directly through text
relocations), so each trampoline derives this front's GOT from a PC thunk into %ecx
(caller-saved and never an argument register for cdecl or for the regparm(1)
___tls_get_addr, whose argument is in %eax), then jumps through the slot (@GOTOFF) or
through the GOT entry of the core function (@GOT). No PLT jump is used: an i386 PLT
entry in a PIC object needs %ebx = the object's GOT.
"""
import argparse
import os
import subprocess
import sys

FRONTS_BY_ARCH = {
    "x86_64": ["libc.so.6", "libm.so.6", "libpthread.so.0", "librt.so.1",
               "libdl.so.2", "ld-linux-x86-64.so.2"],
    "x86": ["libc.so.6", "libm.so.6", "libpthread.so.0", "librt.so.1",
            "libdl.so.2", "ld-linux.so.2"],
}
# Bionic version names (DESIGN.md "Why every front carries dummy LIBC* version
# definitions").
BIONIC_VERSIONS = ["LIBC", "LIBC_N", "LIBC_O", "LIBC_P", "LIBC_Q", "LIBC_R", "LIBC_S",
                   "LIBC_T", "LIBC_DEPRECATED", "LIBC_PLATFORM", "LIBC_PRIVATE"]
STUB_LIBS = [("libc", "NVG_LIB_LIBC"), ("libm", "NVG_LIB_LIBM"), ("libdl", "NVG_LIB_LIBDL")]
# FILE* objects whose value bionic never changes after startup; any other data
# import would need a live alias, not a copy, and is rejected.
DATA_OK = {"stdin", "stdout", "stderr"}
CORE_API = ["nvg_front_init", "nvg_unresolved_trap"]


def read_stubs(nm, stubdir):
    syms = {}
    for lib, enum in STUB_LIBS:
        path = os.path.join(stubdir, lib + ".so")
        out = subprocess.run([nm, "-D", "--defined-only", "-S", path], check=True,
                             capture_output=True, text=True).stdout
        for line in out.splitlines():
            parts = line.split()
            if len(parts) == 4:
                _, size, typ, name = parts
            elif len(parts) == 3:
                _, typ, name = parts
                size = "0"
            else:
                continue
            base = name.split("@", 1)[0]
            if typ in "TWti":
                kind = "func"
            elif typ in "BDVRbdr":
                kind = "object"
            else:
                raise SystemExit(f"gen-fronts: {path}: {name}: unexpected nm type {typ}")
            if base in syms and syms[base][0] != lib:
                raise SystemExit(f"gen-fronts: {base} defined in both {syms[base][0]} and {lib}")
            syms[base] = (lib, enum, kind, int(size, 16))
    return syms


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--census", required=True)
    ap.add_argument("--dispositions", required=True)
    ap.add_argument("--nm", required=True)
    ap.add_argument("--stubdir", required=True)
    ap.add_argument("--outdir", required=True)
    ap.add_argument("--override", help="per-arch rows applied on top of --dispositions")
    ap.add_argument("--arch", default="x86_64", choices=sorted(FRONTS_BY_ARCH))
    a = ap.parse_args()
    FRONTS = FRONTS_BY_ARCH[a.arch]

    disp = {}
    for path in [a.dispositions] + ([a.override] if a.override else []):
        with open(path) as f:
            f.readline()
            for line in f:
                name, d, target, runtime = line.rstrip("\n").split("\t")
                disp[name] = (d, target)
    fronts = {fr: [] for fr in FRONTS}
    with open(a.census) as f:
        f.readline()
        for line in f:
            fr, name, vers, blobs = line.rstrip("\n").split("\t")
            fronts[fr].append(name)
    stubs = read_stubs(a.nm, a.stubdir)

    errors = []
    core_exports = set(CORE_API)
    plan = {}
    for fr in FRONTS:
        rows = []
        for name in sorted(fronts[fr]):
            if name not in disp:
                errors.append(f"{fr}: {name} has no disposition")
                continue
            d, target = disp[name]
            if target.startswith("nvg_"):
                rows.append((name, target, "NVG_LIB_CORE", "CORE"))
                core_exports.add(target)
                continue
            if target not in stubs:
                errors.append(f"{fr}: bionic target {target} (for {name}) is not in the NDK "
                              f"API-33 libc/libm/libdl stubs")
                continue
            lib, enum, kind, size = stubs[target]
            if kind == "object":
                if name not in DATA_OK:
                    errors.append(f"{fr}: {name} -> bionic object {target}: data imports other "
                                  f"than {sorted(DATA_OK)} need a design decision")
                    continue
                # Object sizes in the NDK stubs are placeholders (stdin/stdout/
                # stderr are 4 bytes apart there); the 8-byte FILE* type is
                # asserted against the bionic header in core/init.c instead.
                rows.append((name, target, enum, "DATA"))
            else:
                rows.append((name, target, enum, "SLOT"))
        plan[fr] = rows
    if errors:
        for e in errors:
            print("gen-fronts: " + e, file=sys.stderr)
        sys.exit(1)

    os.makedirs(a.outdir, exist_ok=True)
    for fr, rows in plan.items():
        if a.arch == "x86_64":
            write_front(a.outdir, fr, rows)
        else:
            write_front_i386(a.outdir, fr, rows)
    with open(os.path.join(a.outdir, "core.exports"), "w") as f:
        for n in sorted(core_exports):
            f.write(n + "\n")
    print("gen-fronts: " + ", ".join(
        f"{fr} {len(r)} ({sum(x[3] == 'SLOT' for x in r)} slot, "
        f"{sum(x[3] == 'CORE' for x in r)} core, {sum(x[3] == 'DATA' for x in r)} data)"
        for fr, r in plan.items()))


def write_front(outdir, fr, rows):
    S = []
    S.append(f"/* Generated by tools/gen-fronts.py: front {fr}. Do not edit. */")
    S.append('\t.section .rodata.nvgfront,"a",@progbits')
    S.append(f'.Lsoname:\n\t.asciz "{fr}"')
    for i, (name, target, lib, kind) in enumerate(rows):
        if kind == "SLOT":
            S.append(f'.Lname_{i}:\n\t.asciz "{name}"')
    # dummy version symbols
    for v in BIONIC_VERSIONS:
        S.append(f"\t.globl\tnvg_v_{v}\n\t.type\tnvg_v_{v},@object\n\t.size\tnvg_v_{v},1")
        S.append(f"nvg_v_{v}:\n\t.byte\t0")
    for i, (name, target, lib, kind) in enumerate(rows):
        if kind == "DATA":
            S.append("\t.data\n\t.p2align 3")
            S.append(f"\t.globl\t{name}\n\t.type\t{name},@object\n\t.size\t{name},8")
            S.append(f"{name}:\n\t.quad\t0")
            S.append(f"\t.globl\t__nvgfront_slot_{i}\n\t.hidden\t__nvgfront_slot_{i}\n"
                     f"\t.set\t__nvgfront_slot_{i}, {name}")
        else:
            S.append("\t.text\n\t.p2align 3")
            S.append(f"\t.globl\t{name}\n\t.type\t{name},@function")
            if kind == "SLOT":
                S.append(f"{name}:\n\tjmpq\t*__nvgfront_slot_{i}(%rip)")
            else:
                S.append(f"{name}:\n\tjmp\t{target}@PLT")
            S.append(f"\t.size\t{name}, .-{name}")
            if kind == "SLOT":
                S.append(f".Ltrap_{i}:\n\tleaq\t.Lname_{i}(%rip), %rdi\n"
                         f"\tleaq\t.Lsoname(%rip), %rsi\n\tjmp\tnvg_unresolved_trap@PLT")
                S.append("\t.data\n\t.p2align 3")
                S.append(f"\t.globl\t__nvgfront_slot_{i}\n\t.hidden\t__nvgfront_slot_{i}")
                S.append(f"__nvgfront_slot_{i}:\n\t.quad\t.Ltrap_{i}")
        S.append(f"\t.globl\t__nvgfront_def_{i}\n\t.hidden\t__nvgfront_def_{i}\n"
                 f"\t.set\t__nvgfront_def_{i}, {name}")
    S.append('\t.section .init_array,"aw",@init_array\n\t.p2align 3\n\t.quad\t__nvgfront_init')
    S.append("\t.text\n\t.p2align 3\n\t.globl\t__nvgfront_init\n\t.hidden\t__nvgfront_init\n"
             "\t.type\t__nvgfront_init,@function\n__nvgfront_init:\n"
             "\tleaq\t__nvgfront_table(%rip), %rdi\n\tjmp\tnvg_front_init@PLT\n"
             "\t.size\t__nvgfront_init, .-__nvgfront_init")
    S.append('\t.section .note.GNU-stack,"",@progbits')
    with open(os.path.join(outdir, fr + ".S"), "w") as f:
        f.write("\n".join(S) + "\n")
    write_front_common(outdir, fr, rows)


def write_front_i386(outdir, fr, rows):
    """i386 front: see the module docstring for why every jump goes through %ecx."""
    S = []
    S.append(f"/* Generated by tools/gen-fronts.py --arch x86: front {fr}. Do not edit. */")
    S.append('\t.section .rodata.nvgfront,"a",@progbits')
    S.append(f'.Lsoname:\n\t.asciz "{fr}"')
    for i, (name, target, lib, kind) in enumerate(rows):
        if kind == "SLOT":
            S.append(f'.Lname_{i}:\n\t.asciz "{name}"')
    for v in BIONIC_VERSIONS:
        S.append(f"\t.globl\tnvg_v_{v}\n\t.type\tnvg_v_{v},@object\n\t.size\tnvg_v_{v},1")
        S.append(f"nvg_v_{v}:\n\t.byte\t0")
    # %ecx <- return address; balanced call/ret keeps the return-stack predictor intact
    S.append("\t.text\n\t.p2align 4\n\t.globl\t__nvgfront_pc_ecx\n\t.hidden\t__nvgfront_pc_ecx\n\t.type\t__nvgfront_pc_ecx,@function\n__nvgfront_pc_ecx:\n"
             "\tmovl\t(%esp), %ecx\n\tret\n\t.size\t__nvgfront_pc_ecx, .-__nvgfront_pc_ecx")
    got = "\tcalll\t__nvgfront_pc_ecx\n\taddl\t$_GLOBAL_OFFSET_TABLE_, %ecx"
    for i, (name, target, lib, kind) in enumerate(rows):
        if kind == "DATA":
            S.append("\t.data\n\t.p2align 2")
            S.append(f"\t.globl\t{name}\n\t.type\t{name},@object\n\t.size\t{name},4")
            S.append(f"{name}:\n\t.long\t0")
            S.append(f"\t.globl\t__nvgfront_slot_{i}\n\t.hidden\t__nvgfront_slot_{i}\n"
                     f"\t.set\t__nvgfront_slot_{i}, {name}")
        else:
            S.append("\t.text\n\t.p2align 4")
            S.append(f"\t.globl\t{name}\n\t.type\t{name},@function")
            if kind == "SLOT":
                S.append(f"{name}:\n{got}\n\tjmpl\t*__nvgfront_slot_{i}@GOTOFF(%ecx)")
            else:
                S.append(f"{name}:\n{got}\n\tjmpl\t*{target}@GOT(%ecx)")
            S.append(f"\t.size\t{name}, .-{name}")
            if kind == "SLOT":
                # cdecl call nvg_unresolved_trap(name, soname); never returns
                S.append(f".Ltrap_{i}:\n{got}\n\tsubl\t$4, %esp\n"
                         f"\tleal\t.Lsoname@GOTOFF(%ecx), %eax\n\tpushl\t%eax\n"
                         f"\tleal\t.Lname_{i}@GOTOFF(%ecx), %eax\n\tpushl\t%eax\n"
                         f"\tcalll\t*nvg_unresolved_trap@GOT(%ecx)\n\tud2")
                S.append("\t.data\n\t.p2align 2")
                S.append(f"\t.globl\t__nvgfront_slot_{i}\n\t.hidden\t__nvgfront_slot_{i}")
                S.append(f"__nvgfront_slot_{i}:\n\t.long\t.Ltrap_{i}")
        S.append(f"\t.globl\t__nvgfront_def_{i}\n\t.hidden\t__nvgfront_def_{i}\n"
                 f"\t.set\t__nvgfront_def_{i}, {name}")
    S.append('\t.section .init_array,"aw",@init_array\n\t.p2align 2\n\t.long\t__nvgfront_init')
    # entry: %esp = 12 mod 16; 8 + one 4-byte argument -> 16-byte aligned at the call
    S.append("\t.text\n\t.p2align 4\n\t.globl\t__nvgfront_init\n\t.hidden\t__nvgfront_init\n"
             f"\t.type\t__nvgfront_init,@function\n__nvgfront_init:\n{got}\n"
             "\tsubl\t$8, %esp\n\tleal\t__nvgfront_table@GOTOFF(%ecx), %eax\n\tpushl\t%eax\n"
             "\tcalll\t*nvg_front_init@GOT(%ecx)\n\taddl\t$12, %esp\n\tretl\n"
             "\t.size\t__nvgfront_init, .-__nvgfront_init")
    S.append('\t.section .note.GNU-stack,"",@progbits')
    with open(os.path.join(outdir, fr + ".S"), "w") as f:
        f.write("\n".join(S) + "\n")
    write_front_common(outdir, fr, rows)


def write_front_common(outdir, fr, rows):
    """Version script, table and expected-export list: identical for every arch."""
    with open(os.path.join(outdir, fr + ".map"), "w") as f:
        f.write(f"/* Generated by tools/gen-fronts.py: {fr}. Real exports are deliberately "
                "unlisted (unversioned global). */\n")
        for v in BIONIC_VERSIONS:
            f.write(f"{v} {{ global: nvg_v_{v}; }};\n")

    C = [f"/* Generated by tools/gen-fronts.py: front {fr}. Data only. */",
         '#include "nvg_front.h"', ""]
    for i, (name, target, lib, kind) in enumerate(rows):
        if kind != "CORE":
            C.append(f'extern void *__nvgfront_slot_{i} __attribute__((visibility("hidden")));')
        C.append(f'extern char __nvgfront_def_{i}[] __attribute__((visibility("hidden")));')
    C.append("")
    C.append("static const struct nvg_front_sym syms[] = {")
    for i, (name, target, lib, kind) in enumerate(rows):
        slot = "0" if kind == "CORE" else f"&__nvgfront_slot_{i}"
        C.append(f'    {{ "{name}", "{target}", {slot}, __nvgfront_def_{i}, {lib}, '
                 f'NVG_KIND_{kind} }},')
    C.append("};")
    C.append("")
    C.append('__attribute__((visibility("hidden"))) const struct nvg_front __nvgfront_table = {')
    C.append(f'    NVG_FRONT_MAGIC, NVG_FRONT_ABI, "{fr}", {len(rows)}u, syms,')
    C.append("};")
    with open(os.path.join(outdir, fr + "_table.c"), "w") as f:
        f.write("\n".join(C) + "\n")

    with open(os.path.join(outdir, fr + ".names"), "w") as f:
        for name, target, lib, kind in rows:
            f.write(f"{name}\t{kind}\n")


if __name__ == "__main__":
    main()
