#!/usr/bin/env python3
"""Build and measure the native backend; save complete local evidence.

Requires device access and previously extracted Android runtime/ files. No
container startup, deployment, system modifications or blob loading is done.
"""
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

native = Path(__file__).resolve().parents[1]
repo = native.parents[2]
evidence = native.parent / "evidence"
ndk = Path(os.environ.get("ANDROID_NDK_HOME", repo / "build/hwcomposer-work/ndk/android-ndk-r29"))
clang = ndk / "toolchains/llvm/prebuilt/linux-x86_64/bin/x86_64-linux-android33-clang"
records = []
def run(args, *, env=None, expected=0):
    args = list(map(str, args))
    result = subprocess.run(args, cwd=repo, env=env, text=True, capture_output=True)
    records.append(dict(command=args, status=result.returncode, stdout=result.stdout, stderr=result.stderr))
    print(result.stdout, end="")
    print(result.stderr, end="")
    assert result.returncode == expected, f"command failed: {args}"
    assert "cleanup:" not in result.stderr, "resource destruction failed"
    return result.stdout

report = {"date": datetime.datetime.now(datetime.timezone.utc).isoformat(), "commands": records}
try:
    driver_text = Path("/proc/driver/nvidia/version").read_text()
    match = re.search(r"\b(\d+\.\d+(?:\.\d+)?)\s+Release Build", driver_text)
    assert match, "could not identify the loaded NVIDIA driver version"
    driver = match[1]
    report["driver"] = driver_text
    headers = repo / "build" / f"ogkm-{driver}"
    assert headers.is_dir(), f"missing public RM headers matching loaded driver: {headers}"
    run(["python3", native/"compiler/build.py", "--target", "host"])
    run(["bash", native / "build-android.sh"])
    run(["cargo", "test", "--offline", "--manifest-path", native / "Cargo.toml"])
    run(["cargo", "clippy", "--offline", "--manifest-path", native / "Cargo.toml", "--all-targets", "--", "-D", "warnings"])
    run(["cargo", "fmt", "--manifest-path", native / "Cargo.toml", "--", "--check"])
    run(["cargo", "build", "--offline", "--manifest-path", native / "Cargo.toml"])
    android = native / "target/x86_64-linux-android/release"
    host = native / "target/debug"
    include = ["-I"+str(headers/p) for p in ["src/common/sdk/nvidia/inc", "src/common/inc",
        "kernel-open/common/inc", "src/nvidia/arch/nvalloc/unix/include"]]
    run(["cc", "-Wall", "-Wextra", "-Werror", *include, native/"tools/abi-check.c", "-o", host/"abi-check"])
    run([clang, "-Wall", "-Wextra", "-Werror", *include, native/"tools/abi-check.c", "-o", android/"abi-check"])
    run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", repo/"tools/native-backend-probe.c", "-ldl", "-o", host/"native-backend-probe"])
    android_env = dict(os.environ, LD_LIBRARY_PATH=str(native/"runtime/lib64"),
        NV_NATIVE_COMPILER_PATH=str(android/"libnv_native_nak.so"))
    host_env = dict(os.environ, NV_NATIVE_COMPILER_PATH=str(repo/"build/nak-host/src/nouveau/native-bridge/libnv_native_nak.so"))
    linker = native/"runtime/linker64"
    c_abi = run([host/"abi-check"])
    rust_abi = run([host/"nv-native-check", "--abi"])
    android_c = run([linker, android/"abi-check"], env=android_env)
    android_rust = run([linker, android/"nv-native-check", "--abi"], env=android_env)
    assert c_abi == rust_abi == android_c == android_rust, "C/Rust/host/Android ABI mismatch"
    report["abi_rows"] = len(c_abi.splitlines())
    (evidence/"native-abi-c.tsv").write_text(c_abi)
    (evidence/"native-abi-rust.tsv").write_text(rust_abi)
    run([host/"nv-native-check", "--unknown"], expected=1)
    run([host/"native-backend-probe", host/"libnv_bionic_native.so"], env=host_env)
    run([linker, android/"native-backend-probe", android/"libnv_bionic_native.so"], env=android_env)
    run([linker, android/"nv-native-check", "--compute"], env=android_env)
    report["tool_versions"] = {"rust": run(["rustc", "--version"]),
        "ndk": (ndk/"source.properties").read_text(),
        "glslang": run(["glslangValidator", "--version"])}
    files = [*native.glob("src/*.rs"), *native.glob("tools/*.*"), native/"Cargo.toml", native/"Cargo.lock",
             native/"build-android.sh", native/"shaders/pattern.comp", native/"shaders/pattern.spv",
             repo/"tools/native-backend-probe.c", android/"nv-native-check", android/"libnv_bionic_native.so",
             android/"native-backend-probe", linker, *native.glob("runtime/lib64/*.so")]
    report["sha256"] = {str(p.relative_to(repo)): hashlib.sha256(p.read_bytes()).hexdigest() for p in files}
    report["passed"] = True
finally:
    (evidence/"native-bionic-verification.json").write_text(json.dumps(report, indent=2)+"\n")
print("PASS: native bionic artifacts, C ABI, hardware results, shared-library loading and fd lifetime")
