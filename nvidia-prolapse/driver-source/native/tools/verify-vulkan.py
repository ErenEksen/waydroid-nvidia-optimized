#!/usr/bin/env python3
"""Build and verify the source-built Vulkan compute path on host and bionic.

Runs the existing independent RM/ABI suite, then public Vulkan tests through
direct host, host Khronos loader, and bionic interfaces. No deployment occurs.
"""
from pathlib import Path
import datetime
import hashlib
import json
import os
import subprocess

native = Path(__file__).resolve().parents[1]
repo = native.parents[2]
evidence = native.parent / "evidence"
out = repo/"build/native-vulkan-probe"
android = native/"target/x86_64-linux-android/release"
host = native/"target/debug"
compiler_host = repo/"build/nak-host/src/nouveau/native-bridge/libnv_native_nak.so"
compiler_android = android/"libnv_native_nak.so"
ndk = Path(os.environ.get("ANDROID_NDK_HOME",repo/"build/hwcomposer-work/ndk/android-ndk-r29"))
started = datetime.datetime.now(datetime.timezone.utc)
report = {"started":started.isoformat(), "commands":[]}

def run(name,args,env=None):
    args=list(map(str,args))
    result=subprocess.run(args,cwd=repo,env=env,text=True,capture_output=True)
    log=result.stdout+result.stderr
    (evidence/f"{name}.log").write_text(log)
    report["commands"].append({"name":name,"command":args,"status":result.returncode,
        "environment":{k:v for k,v in (env or {}).items() if k in ("NV_NATIVE_COMPILER_PATH","VK_DRIVER_FILES","VK_LOADER_LAYERS_DISABLE","LD_LIBRARY_PATH")},
        "stdout":result.stdout,"stderr":result.stderr})
    print(f"{name}: exit {result.returncode}",flush=True)
    assert result.returncode==0, f"{name} failed; read {evidence/name}.log"
    assert not any(s in log for s in ("cleanup:","worker panicked","munmap NVIDIA allocation:")), "native teardown error"
    return log

try:
    out.mkdir(parents=True,exist_ok=True)
    run("vulkan-backend-regression",["python3",native/"tools/verify.py"])
    for name,cc in [("host","cc"),("android",ndk/"toolchains/llvm/prebuilt/linux-x86_64/bin/x86_64-linux-android33-clang")]:
        run(f"vulkan-probe-build-{name}",[cc,"-std=c11","-Wall","-Wextra","-Werror",repo/"tools/native-vulkan-probe.c","-ldl","-o",out/f"probe-{name}"])
    for shader in ("generate","transform"):
        run(f"vulkan-glsl-{shader}",["glslangValidator","-V","--target-env","vulkan1.0",repo/f"tools/native-vulkan-shaders/{shader}.comp","-o",out/f"{shader}.spv"])
        run(f"vulkan-spirv-{shader}",["spirv-val","--target-env","vulkan1.0",out/f"{shader}.spv"])
    args=["--shader",out/"generate.spv","--transform",out/"transform.spv","--negative"]
    host_env=dict(os.environ,NV_NATIVE_COMPILER_PATH=str(compiler_host))
    android_env=dict(os.environ,NV_NATIVE_COMPILER_PATH=str(compiler_android),LD_LIBRARY_PATH=str(native/"runtime/lib64"))
    manifest=native/"native_icd.json"
    manifest.write_text(json.dumps({"file_format_version":"1.0.0","ICD":{"library_path":str(host/"libnv_bionic_native.so"),"api_version":"1.0.0"}},indent=2)+"\n")
    loader_env=dict(host_env,VK_DRIVER_FILES=str(manifest),VK_LOADER_LAYERS_DISABLE="~implicit~")
    cases=[("vulkan-native-host",[out/"probe-host","--driver",host/"libnv_bionic_native.so",*args],host_env),
        ("vulkan-native-loader-host",[out/"probe-host","--loader","--driver","/usr/lib/libvulkan.so.1","--negative",*args[:-1]],loader_env),
        ("vulkan-native-bionic",[native/"runtime/linker64",out/"probe-android","--driver",android/"libnv_bionic_native.so",*args],android_env)]
    for name,command,env in cases:
        log=run(name,command,env)
        assert "4422 output words" in log and "file descriptors before/after: 4/4" in log
        assert sum(line.startswith("round ") for line in log.splitlines())==6
        if name.endswith("bionic"):
            assert str(native/"runtime/lib64/libc.so") in log and "libc.so.6" not in log
    for path in (compiler_android,android/"libnv_bionic_native.so"):
        metadata=run(path.stem+"-android-elf",["readelf","-d","--version-info",path])
        assert "GLIBC_" not in metadata and "libc.so.6" not in metadata
        assert "libnvidia-" not in metadata and "libnvglibc" not in metadata
    files=[p for base in (native/"src",native/"compiler",native/"tools",native/"shaders") for p in base.rglob("*")
        if p.is_file() and p.suffix in (".rs",".h",".c",".py",".sh",".comp",".spv",".patch")]
    files += [native/"Cargo.toml",native/"Cargo.lock",native/"build-android.sh",repo/"tools/native-vulkan-probe.c",
        *list((repo/"tools/native-vulkan-shaders").glob("*.comp")),host/"libnv_bionic_native.so",android/"libnv_bionic_native.so",
        compiler_host,compiler_android,out/"probe-host",out/"probe-android",out/"generate.spv",out/"transform.spv"]
    report["sha256"]={str(p.relative_to(repo)):hashlib.sha256(p.read_bytes()).hexdigest() for p in files}
    journal=subprocess.run(["journalctl","-k","--since",started.strftime("%Y-%m-%d %H:%M:%S UTC"),"--grep=Xid","--no-pager"],text=True,capture_output=True)
    report["kernel_xid_check"]={"status":journal.returncode,"stdout":journal.stdout,"stderr":journal.stderr}
    assert journal.returncode in (0,1) and "Xid" not in journal.stdout and not journal.stderr
    report["passed"]=True
finally:
    report["finished"]=datetime.datetime.now(datetime.timezone.utc).isoformat()
    (evidence/"vulkan-source-verification.json").write_text(json.dumps(report,indent=2)+"\n")
print("PASS: real public Vulkan compute/copy under host loader and bionic; source compiler; no NVIDIA userspace/shim")
