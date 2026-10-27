#!/usr/bin/env python3
"""Find the installed NVIDIA driver libraries the shims are built for (run with `python3 -I`).

    find-driver.py --arch x86_64|x86 [--proc /proc/driver/nvidia/version] [--cache /etc/ld.so.cache]

Prints the paths of the driver's Vulkan ICD closure, one per line, for the loaded kernel module's
version. The libraries are looked up the way the dynamic loader finds them: by soname in the
loader's cache (/etc/ld.so.cache, written by ldconfig), with the cache entry's ELF class selecting
the architecture, so the distribution's layout does not matter (/usr/lib, /usr/lib32, /usr/lib64,
/usr/lib/x86_64-linux-gnu, ...).

The closure (DESIGN.md "Driver branches") is libEGL_nvidia.so.0 (the ICD), the two libraries the
driver dlopens (libnvidia-eglcore, dlopened by libEGL_nvidia; libnvidia-glvkspirv, dlopened by
eglcore), and every libnvidia-*.so.<version> in their DT_NEEDED, transitively (glsi everywhere,
gpucomp from 550 on). Every file must carry the kernel module's version exactly; a missing file or
another version fails with the file name and the remedy.
"""
import argparse
import mmap
import os
import re
import struct
import sys

PROC_VERSION = "/proc/driver/nvidia/version"
LD_SO_CACHE = "/etc/ld.so.cache"
ICD_SONAME = "libEGL_nvidia.so.0"
DLOPEN_ROOTS = ("libnvidia-eglcore", "libnvidia-glvkspirv")
# glibc elf/cache.h: FLAG_ELF_LIBC6 | FLAG_X8664_LIB64 for x86_64, FLAG_ELF_LIBC6 alone for i386
CACHE_FLAGS = {"x86_64": 0x0303, "x86": 0x0003}
ELF_CLASS = {"x86_64": 2, "x86": 1}


def die(msg):
    sys.exit("find-driver: " + msg)


def driver_version(path):
    try:
        text = open(path).read()
    except OSError as e:
        die(f"cannot read {path} ({e.strerror}); is the NVIDIA kernel module loaded?")
    m = re.search(r"^NVRM version:.*?\s(\d+\.\d+(?:\.\d+)?)\s", text, re.M)
    if not m:
        die(f"no 'NVRM version: ... <x.y.z>' line in {path}")
    return m.group(1)


def read_cache(path):
    """[(flags, soname, path)] from glibc's ld.so.cache (new format, alone or after the old one)."""
    try:
        d = open(path, "rb").read()
    except OSError as e:
        die(f"cannot read the loader cache {path} ({e.strerror}); run ldconfig")
    base = 0
    if d.startswith(b"ld.so-1.7.0"):
        (n_old,) = struct.unpack_from("<I", d, 12)
        base = (16 + 12 * n_old + 7) & ~7
    if d[base:base + 20] != b"glibc-ld.so.cache1.1":
        die(f"{path} is not a glibc ld.so.cache (format 1.1); run ldconfig")
    (nlibs,) = struct.unpack_from("<I", d, base + 20)
    out = []
    for i in range(nlibs):
        flags, key, value, _, _ = struct.unpack_from("<iIIIQ", d, base + 48 + 24 * i)
        k = d[base + key:d.index(b"\0", base + key)].decode()
        v = d[base + value:d.index(b"\0", base + value)].decode()
        out.append((flags, k, v))
    return out


def elf_needed(path, arch):
    """DT_NEEDED names of an ELF shared object of `arch` (raises SystemExit on another class)."""
    with open(path, "rb") as f:
        d = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)  # read lazily: gpucomp is > 100 MB
    if d[:4] != b"\x7fELF" or d[4] != ELF_CLASS[arch] or d[5] != 1:
        die(f"{path} is not a little-endian ELF{64 if arch == 'x86_64' else 32} file")
    if arch == "x86_64":
        phoff, = struct.unpack_from("<Q", d, 0x20)
        phentsize, phnum = struct.unpack_from("<HH", d, 0x36)
        ph, dyn = "<IIQQQQQQ", "<qQ"
    else:
        phoff, = struct.unpack_from("<I", d, 0x1c)
        phentsize, phnum = struct.unpack_from("<HH", d, 0x2a)
        ph, dyn = "<IIIIIIII", "<iI"
    loads, dynamic = [], None
    for i in range(phnum):
        f = struct.unpack_from(ph, d, phoff + i * phentsize)
        if arch == "x86_64":
            p_type, _, p_offset, p_vaddr, _, p_filesz = f[0], f[1], f[2], f[3], f[4], f[5]
        else:
            p_type, p_offset, p_vaddr, _, p_filesz = f[0], f[1], f[2], f[3], f[4]
        if p_type == 1:
            loads.append((p_vaddr, p_offset, p_filesz))
        elif p_type == 2:
            dynamic = (p_offset, p_filesz)
    if dynamic is None:
        die(f"{path} has no PT_DYNAMIC")
    size = struct.calcsize(dyn)
    entries = [struct.unpack_from(dyn, d, dynamic[0] + i * size) for i in range(dynamic[1] // size)]
    strtab = next((v for t, v in entries if t == 5), None)
    if strtab is None:
        die(f"{path} has no DT_STRTAB")
    off = next((o + strtab - va for va, o, sz in loads if va <= strtab < va + sz), None)
    if off is None:
        die(f"{path}: DT_STRTAB is outside every PT_LOAD")
    return [d[off + v:d.find(b"\0", off + v)].decode() for t, v in entries if t == 1]


def closure(arch, version, cache):
    by_name = {}
    for flags, soname, path in cache:
        if flags == CACHE_FLAGS[arch]:
            by_name.setdefault(soname, path)
    icd = by_name.get(ICD_SONAME)
    if icd is None:
        die(f"{ICD_SONAME} ({arch}) is not in the loader cache: the NVIDIA {'32-bit ' if arch == 'x86' else ''}"
            f"driver libraries of {version} are not installed (or run ldconfig)")
    real = os.path.realpath(icd)
    if os.path.basename(real) != f"libEGL_nvidia.so.{version}":
        die(f"{icd} ({arch}) resolves to {real}, not libEGL_nvidia.so.{version} of the loaded kernel module "
            f"{version}: the driver's userspace and kernel module differ (reboot after a driver update)")
    found = {os.path.basename(real): real}
    want = elf_needed(real, arch) + [f"{r}.so.{version}" for r in DLOPEN_ROOTS]
    want = [n for n in want if n.startswith("libnvidia-")]
    while want:
        name = want.pop(0)
        if name in found:
            continue
        path = by_name.get(name)
        if path is None:
            others = sorted(s for s in by_name if s.startswith(name.rsplit(".so.", 1)[0] + ".so."))
            die(f"{name} ({arch}) is not in the loader cache although the kernel module is {version}"
                + (f" (the cache has {', '.join(others)})" if others else "")
                + ": reinstall the NVIDIA driver's userspace of that version")
        found[name] = path
        want += [n for n in elf_needed(path, arch) if n.startswith("libnvidia-")]
    for name in found:
        if not name.endswith(".so." + version):
            die(f"{found[name]} ({arch}) is not of driver {version}")
    return [found[n] for n in found]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--arch", required=True, choices=sorted(CACHE_FLAGS))
    ap.add_argument("--proc", default=PROC_VERSION)
    ap.add_argument("--cache", default=LD_SO_CACHE)
    a = ap.parse_args()
    for path in closure(a.arch, driver_version(a.proc), read_cache(a.cache)):
        print(path)


if __name__ == "__main__":
    main()
