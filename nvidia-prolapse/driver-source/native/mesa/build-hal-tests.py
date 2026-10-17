#!/usr/bin/env python3
"""Cross-build the independent public ANB/ASTC HAL tests for both Android ABIs."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[4]
HAL = ROOT / "nvidia-prolapse/hal"
NDK = Path("/opt/android-ndk/toolchains/llvm/prebuilt/linux-x86_64/bin")
flags = ["-std=c11", "-D_GNU_SOURCE", "-DVK_NO_PROTOTYPES", "-DVK_USE_PLATFORM_ANDROID_KHR",
         "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-Isrc", "-Iinclude/android",
         "-Iinclude/minigbm", "-Iout/gen"]
# generated inputs of the tests (Mesa's ASTC reference decoder at the HAL's pinned MESA_SHA, sampling shader)
subprocess.run(["make", "out/gen/mesa/src/mesa/main/texcompress_astc.cpp", "out/gen/astc_sample_spv.h"],
               cwd=HAL, check=True)
for target, triple in [("android", "x86_64-linux-android"), ("android32", "i686-linux-android")]:
    out = ROOT / "build/source-native-integration" / target
    out.mkdir(parents=True, exist_ok=True)
    cc = str(NDK / (triple + "33-clang"))
    cxx = str(NDK / (triple + "33-clang++"))
    commands = [
        [cc, *flags, "test/anb-roundtrip.c", "-ldl", "-o", out / "anb-roundtrip"],
        [cxx, "-std=c++17", "-O1", "-g", "-Itest/astc-ref", "-Iout/gen/mesa/src/mesa/main",
         "-c", "test/astc-ref.cpp", "-o", out / "astc-ref.o"],
        [cc, *flags, "-c", "test/astc-roundtrip.c", "-o", out / "astc-roundtrip.o"],
        [cxx, out / "astc-roundtrip.o", out / "astc-ref.o", "-ldl", "-static-libstdc++",
         "-o", out / "astc-roundtrip"],
    ]
    for command in commands:
        subprocess.run([str(value) for value in command], cwd=HAL, check=True)
    print(f"Built {target} ANB and ASTC probes", flush=True)
