#!/usr/bin/env python3
"""Check the built Android libraries by looking at them (llvm-readelf), not at the sources.

Per library and ABI: ELF class, DT_SONAME == file name, DT_NEEDED within the allowed bionic set, no glibc
version reference (GLIBC_*) and no glibc SONAME, and the exact dynamic export set:
  libEGL_nvnative.so          the forwarded loader entries (front_entries.h) + the front's own entries (FRONT_OWN)
  libGLESv1_CM/v2_nvnative.so nothing (and never ANGLEGetDisplayPlatform, Loader.cpp:555-564)
  libnvnative-egl-android.so  loadEGLExternalPlatform only
"""
import argparse
import re
import subprocess
import sys

FRONT_OWN = {"eglGetDisplay", "eglGetPlatformDisplay", "eglGetProcAddress", "eglGetError", "eglQueryString",
             "eglMakeCurrent", "eglBindAPI", "eglChooseConfig", "eglGetConfigAttrib"}


def readelf(rd, *args):
    return subprocess.run([rd, *args], check=True, capture_output=True, text=True).stdout


def exports(rd, path):
    out = set()
    for line in readelf(rd, "--dyn-syms", "--wide", path).splitlines():
        f = line.split()
        if len(f) >= 8 and f[0].endswith(":") and f[0][:-1].isdigit() and f[6] != "UND" and f[4] in ("GLOBAL", "WEAK") \
                and f[5] == "DEFAULT":
            out.add(f[7].split("@")[0])
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--readelf", required=True)
    ap.add_argument("--entries", required=True)
    ap.add_argument("--allowed", required=True)
    ap.add_argument("dirs", nargs="+", help="DIR:ELF64|ELF32")
    a = ap.parse_args()
    forwarded = set(re.findall(r"X\([^,]+, (egl\w+),", open(a.entries).read()))
    want = {
        "libEGL_nvnative.so": forwarded | FRONT_OWN,
        "libGLESv1_CM_nvnative.so": set(),
        "libGLESv2_nvnative.so": set(),
        "libnvnative-egl-android.so": {"loadEGLExternalPlatform"},
    }
    allowed = set(a.allowed.split())
    errors = 0
    for spec in a.dirs:
        d, cls = spec.split(":")
        for lib, exp in want.items():
            p = f"{d}/{lib}"
            hdr = readelf(a.readelf, "-h", p)
            dyn = readelf(a.readelf, "-d", p)
            ver = readelf(a.readelf, "-V", p)
            soname = re.findall(r"\(SONAME\).*\[(.*)\]", dyn)
            needed = re.findall(r"\(NEEDED\).*\[(.*)\]", dyn)
            got = exports(a.readelf, p)
            errs = []
            if f"Class:" not in hdr or cls not in hdr:
                errs.append(f"not {cls}")
            if soname != [lib]:
                errs.append(f"SONAME {soname}")
            bad = [n for n in needed if n not in allowed]
            if bad:
                errs.append(f"DT_NEEDED outside the bionic set: {bad}")
            if "GLIBC_" in ver or any(n.startswith(("libc.so.6", "libm.so.6", "libpthread", "libdl.so.2")) for n in needed):
                errs.append("references glibc")
            if got != exp:
                errs.append(f"exports differ: missing {sorted(exp - got)[:8]}, extra {sorted(got - exp)[:8]}")
            if "ANGLEGetDisplayPlatform" in got:
                errs.append("exports ANGLEGetDisplayPlatform")
            status = "OK" if not errs else "FAIL: " + "; ".join(errs)
            print(f"check: {p}: {cls}, NEEDED {needed}, {len(got)} exports: {status}")
            errors += bool(errs)
    if errors:
        sys.exit(f"check: {errors} library(ies) failed")
    print(f"check: OK ({len(forwarded)} forwarded + {len(FRONT_OWN)} front EGL entries)")


if __name__ == "__main__":
    main()
