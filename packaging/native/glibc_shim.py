#!/usr/bin/env python3
"""glibc-shim mode for package users: check this host's NVIDIA driver against the release's prebuilt glibc-shim
stack and stage it (waydroid-nvidia-setup --mode glibc-shim runs this as root, the container stopped).

  glibc_shim.py check --bundle DIR [--native-gl on|off]
  glibc_shim.py stage --bundle DIR [--root ROOT] [--native-gl on|off] [--tag TAG]

DIR is the release bundle (/usr/lib/waydroid-nvidia/native/release). Its glibc-shim/ tree (BUILD-RECORD.json
"glibc_shim_files", sha256 each) holds the guest side, built from source by packaging/native/build-native.sh:
  glibc-shim/root/vendor_extra/lib{64,}/   the glibc-compat shims (libnvglibc.so + six glibc-named fronts), the
                                           Vulkan HAL hw/vulkan.nvidia.so, the gralloc backend and minigbm core,
                                           libdrm.so.2, NVIDIA's EGL platform for Android + its JSON and the
                                           shims' default environment
  glibc-shim/root/vendor/lib{64,}/egl/     the EGL front lib{EGL,GLESv1_CM,GLESv2}_nvnative.so
  glibc-shim/check/                        what the build exports: census-<arch>.tsv (every glibc name the shims
                                           export, per front) and libdrm-<arch>.names (every drm* name libdrm.so.2
                                           exports)
NVIDIA's own libraries never come from the package: they are the host's, bind-mounted at container start from where
the dynamic loader finds them (waydroid/nvidia_native.py host_closure()).

check (also the first half of stage) refuses, naming the reason and the remedy, when
  - the bundle's glibc-shim files differ from the build record;
  - the loaded driver is older than 535 (no dma-buf / modifier Vulkan extensions), or its libraries are not
    installed at the kernel module's version for either ABI (the 32-bit ones name the distribution's package);
  - any glibc name the host's driver libraries import is not exported by the prebuilt shims (a driver release
    newer than this package: nvidia-prolapse/shim/tools/import-census.py --covered-by), or any drm* name they
    reference is not exported by the prebuilt libdrm.so.2;
  - --native-gl on and the driver lacks what NVIDIA's GLES through the EGL front needs (535/550).
stage then writes ROOT (default /var/lib/waydroid/nvidia-glibc-shim-staging): the files above (new inodes), the
shims' host-glibc-version sidecar, the libEGL_nvidia.so.0 soname link and an empty placeholder per host library
(the container binds the host file over it), runs the LXC generator on it, then the gate: the release's Vulkan gate
programs (compute, graphics, dma-buf/sync-fd interop, the HAL's ANB/ASTC/ETC2 round trips; both ABIs) on this GPU with
the bionic runtime of the user's system.img, each process required to map NVIDIA's driver through the shims
(--expect-driver glibc-shim); logs in /var/lib/waydroid-nvidia/gate-glibc-shim-<time>. Only when every case passes
is the glibc-shim root link (/var/lib/waydroid/nvidia-glibc-shim) pointed at ROOT.
"""
import argparse
import hashlib
import importlib.machinery
import importlib.util
import json
import mmap
import os
from pathlib import Path
import re
import subprocess
import sys

HERE = Path(__file__).resolve().parent
NATIVE = HERE.parents[1]            # the source tree, or /usr/lib/waydroid-nvidia/native in a package
TREE = 'glibc-shim'
RECORD_KEY = 'glibc_shim_files'
ARCHES = {64: ('x86_64', 'lib64'), 32: ('x86', 'lib')}
SHIM_DIR = NATIVE / 'nvidia-prolapse/shim'
DRM_NAME = re.compile(rb'drm[A-Z][A-Za-z0-9_]+')
PRINTABLE = re.compile(rb'[\x20-\x7e\t]{4,}')        # a printable run as `strings -a` sees it
SETUP = 'sudo waydroid-nvidia-setup --mode glibc-shim'


def load(path, name):
    loader = importlib.machinery.SourceFileLoader(name, str(path))
    spec = importlib.util.spec_from_loader(name, loader)
    module = importlib.util.module_from_spec(spec)
    loader.exec_module(module)
    return module


native = load(NATIVE / 'waydroid/nvidia_native.py', 'nvidia_native')


class Refused(Exception):
    pass


def sha256(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


def verify_tree(bundle):
    """{relative path under glibc-shim/: path} after checking the tree against BUILD-RECORD.json."""
    try:
        record = json.loads((bundle / 'BUILD-RECORD.json').read_text())
    except (OSError, ValueError) as error:
        raise Refused(f'release bundle {bundle} unreadable ({error}); reinstall the package')
    want = record.get(RECORD_KEY)
    if not isinstance(want, dict) or not want:
        raise Refused(f'release bundle {bundle} carries no glibc-shim build ({RECORD_KEY} missing from '
                      f'BUILD-RECORD.json): this release does not ship glibc-shim mode')
    base = bundle / TREE
    have = {str(p.relative_to(bundle)): p for p in sorted(base.rglob('*')) if p.is_file() or p.is_symlink()}
    problems = [f'missing {r}' for r in sorted(set(want) - set(have))]
    problems += [f'not in the build record: {r}' for r in sorted(set(have) - set(want))]
    problems += [f'modified: {r}' for r in sorted(set(want) & set(have)) if have[r].is_symlink() or sha256(have[r]) != want[r]]
    if problems:
        raise Refused(f'the glibc-shim files of {bundle} do not match its build record ({len(problems)} problems; '
                      f'first: {problems[0]}); reinstall the package')
    for rel, path in have.items():
        if rel.endswith('.so') or '.so.' in rel:
            bits = 32 if '/lib/' in rel else 64
            if native._elf_class(str(path)) != bits:
                raise Refused(f'{path} is not an ELF{bits} library; reinstall the package')
    return {str(Path(r).relative_to(TREE)): p for r, p in have.items()}


def drm_names(paths):
    """{drm* name: [library]} for every drm* string the given libraries carry."""
    seen = {}
    for p in paths:
        with open(p, 'rb') as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as d:
            for run in PRINTABLE.findall(d):
                if DRM_NAME.fullmatch(run):
                    seen.setdefault(run.decode(), []).append(os.path.basename(p))
    return seen


def imports_not_covered(arch, census, paths):
    """'' when every glibc import of the libraries `paths` is exported by the shim build behind `census`, else
    import-census.py's report (nvidia-prolapse/shim/tools/import-census.py --covered-by)."""
    cmd = [sys.executable, '-I', str(SHIM_DIR / 'tools/import-census.py'), '--arch', arch,
           '--dispositions', str(SHIM_DIR / 'dispositions.tsv')]
    if arch == 'x86':
        cmd += ['--override', str(SHIM_DIR / 'dispositions-x86.tsv')]
    proc = subprocess.run(cmd + ['--covered-by', str(census)] + paths, capture_output=True, text=True)
    return '' if proc.returncode == 0 else (proc.stderr or proc.stdout).strip() or f'import-census exit {proc.returncode}'


def check(bundle, native_gl):
    """(driver version, {bits: [(soname, host path)]}, tree files) or Refused naming why."""
    files = verify_tree(bundle)
    v = native.driver_version()
    closures = {}
    for bits, (arch, sub) in ARCHES.items():
        closure = native.host_closure(v, bits)
        closures[bits] = closure
        paths = [p for _, p in closure]
        uncovered = imports_not_covered(arch, files[f'check/census-{arch}.tsv'], paths)
        if uncovered:
            raise Refused(f'driver {v} ({bits}-bit) imports glibc functions the glibc-shim build of this package does '
                          f'not provide; it was built for older driver releases. Update waydroid-nvidia, or use the '
                          f'default mode (sudo waydroid-nvidia-setup --mode nvidia-prolapse) meanwhile:\n' + uncovered)
        have = {l.strip() for l in files[f'check/libdrm-{arch}.names'].read_text().splitlines() if l.strip()}
        missing = sorted(n for n in drm_names(paths) if n not in have)
        if missing:
            raise Refused(f'driver {v} ({bits}-bit) references libdrm functions the libdrm.so.2 of this package does '
                          f'not export ({" ".join(missing)}); update waydroid-nvidia')
        if native_gl:
            native.check_nvnative_driver(paths[0])
        print(f'glibc-shim: driver {v} {bits}-bit: {len(paths)} libraries ({", ".join(n for n, _ in closure)}), '
              f'every import covered')
    return v, closures, files


def gate(bundle, root, closures, images=Path('/var/lib/waydroid/images'), log_root=Path('/var/lib/waydroid-nvidia')):
    """Run the release's Vulkan gate programs on this GPU against the staged tree and the host's driver, both ABIs,
    with the bionic runtime of the user's system.img (packaging/native/release_gate.py), expecting NVIDIA's driver
    through the shims in every process (--expect-driver glibc-shim). Raises RuntimeError naming the failing case."""
    rg = load(HERE / 'release_gate.py', 'release_gate')
    system_img = Path(images) / 'system.img'
    if not system_img.is_file():
        raise Refused(f'{system_img} missing: run `waydroid init` first')
    work = Path(log_root) / f'gate-glibc-shim-{rg.time.strftime("%Y%m%d-%H%M%S")}'
    work.mkdir(parents=True)
    # what the guest sees in /vendor_extra/<lib>: the staged files, with the host's driver libraries over the
    # placeholders (the container binds them there)
    lib = work / 'vendor_extra'
    for bits, (_, sub) in ARCHES.items():
        hosts = dict(closures[bits])
        for path in sorted((root / 'vendor_extra' / sub).rglob('*')):
            rel = path.relative_to(root / 'vendor_extra' / sub)
            dest = lib / sub / rel
            if path.is_dir():
                dest.mkdir(parents=True, exist_ok=True)
                continue
            dest.parent.mkdir(parents=True, exist_ok=True)
            if path.is_symlink():
                dest.symlink_to(os.readlink(path))
            else:
                dest.symlink_to(hosts.get(path.name, path))
    print(f'glibc-shim gate on driver {native.driver_version()}; logs in {work}', flush=True)

    class ShimGate(rg.Gate):
        def __init__(self):
            super().__init__(bundle, work, system_img)
            self.payload = work

        def prepare(self, target):
            sub = rg.ABIS[target][0]
            dirs = [lib / sub]
            provided = {p.name for p in (lib / sub).iterdir()}
            # only the programs this gate runs: the release's GL and media programs need our GL driver and the
            # Codec2 core, which glibc-shim mode does not stage
            roots, todo, seen = set(rg.RUNTIME_ROOTS), [*(self.progs / 'bin' / target / p for p in rg.VULKAN_PROGRAMS),
                                                        lib / sub / native.ICD_SONAME, lib / sub / 'hw/vulkan.nvidia.so'], set()
            while todo:
                obj = todo.pop()
                if obj in seen:
                    continue
                seen.add(obj)
                for name in rg.needed(obj, leaf_ok=True):
                    local = next((d / name for d in dirs if (d / name).exists()), None)
                    if local is not None:
                        todo.append(local)
                    else:
                        roots.add(name)
            out = work / 'runtime' / sub
            origin = rg.extract_runtime(system_img, out, sub, sorted(roots), provided)
            self.runtime[target] = out
            return origin

        def libpath(self, target):
            sub = rg.ABIS[target][0]
            return [lib / sub, root / 'vendor' / sub / 'egl', self.runtime[target]]

    g = ShimGate()
    for target in rg.ABIS:
        g.prepare(target)
    for target in rg.ABIS:
        rg.vulkan_cases(g, target, 'glibc-shim', lib / rg.ABIS[target][0], native.ICD_SONAME)
    (work / 'gate.json').write_text(json.dumps({'mode': 'glibc-shim', 'driver': native.driver_version(),
                                                'cases': g.cases}, indent=2) + '\n')
    print(f'glibc-shim gate: {len(g.cases)} cases passed', flush=True)
    return g.cases


def glibc_version():
    out = subprocess.run(['getconf', 'GNU_LIBC_VERSION'], capture_output=True, text=True).stdout.split()
    if len(out) != 2 or not re.fullmatch(r'\d+\.\d+', out[1]):
        raise Refused(f'getconf GNU_LIBC_VERSION gave {" ".join(out)!r}, not "glibc <major>.<minor>"')
    return out[1]


def put(src, dst):
    """Install src at dst as a new inode (tmp + rename), root-owned 0644: a running process keeps its mapping."""
    dst.parent.mkdir(parents=True, exist_ok=True)
    tmp = dst.with_name(dst.name + '.tmp')
    tmp.write_bytes(src.read_bytes())
    tmp.chmod(0o644)
    os.replace(tmp, dst)


def mounted(root):
    with open('/proc/self/mountinfo') as f:
        text = f.read()
    return f' {root}/vendor_extra ' in text or '/vendor_extra/lib64/libnvidia-' in text


def stage(bundle, root, native_gl, tag, run_gate=True):
    v, closures, files = check(bundle, native_gl)
    if os.geteuid() != 0:
        raise Refused('stage needs root (it writes under /var/lib/waydroid)')
    if mounted(root):
        raise Refused(f'{root} is in use by a running Waydroid container; stop it first (waydroid session stop; '
                      f'sudo systemctl stop waydroid-container)')
    glibc = glibc_version()
    for rel, path in files.items():
        if rel.startswith('root/'):
            put(path, root / rel[len('root/'):])
    for bits, (_, sub) in ARCHES.items():
        lib = root / 'vendor_extra' / sub
        side = lib / 'host-glibc-version'
        tmp = side.with_name(side.name + '.tmp')
        tmp.write_text(glibc + '\n')
        tmp.chmod(0o644)
        os.replace(tmp, side)
        want = {name for name, _ in closures[bits]}
        for name in sorted(want):
            if not (lib / name).exists():
                (lib / name).touch(0o644)
        link = lib / native.ICD_SONAME
        if link.is_symlink() or link.exists():
            link.unlink()
        link.symlink_to(native.ICD_FILE.format(v=v))
        for old in sorted(lib.glob('libEGL_nvidia.so.[0-9]*')) + sorted(lib.glob('libnvidia-*.so.[0-9]*')):
            if old.is_symlink() or old.name in want:
                continue
            if old.stat().st_size:
                raise Refused(f'refusing to remove {old}: not an empty placeholder')
            old.unlink()
            print(f'glibc-shim: removed the placeholder {old} of another driver version')
        for egl in native.EGL_ANGLE:
            p = root / 'vendor' / sub / 'egl' / egl
            if not p.exists():
                p.parent.mkdir(parents=True, exist_ok=True)
                p.touch(0o644)
    for d in [root] + [p for p in root.rglob('*') if p.is_dir()]:
        d.chmod(0o755)
    (root / 'staged-build-info').write_text(f'package {tag or "unknown"} glibc-shim driver {v} glibc {glibc}\n')
    entries = native.lxc_nodes(str(root), native_gl=native_gl)
    if run_gate:
        try:
            gate(bundle, root, closures)
        except RuntimeError as error:
            raise Refused(f'the glibc-shim gate failed on this GPU and driver; the mode was not switched:\n{error}')
    previous = native.repoint_link(native.ROOT_LINKS['glibc-shim'], str(root))
    print(f'glibc-shim: staged {root} for driver {v} (glibc {glibc}; generator: {len(entries)} mount entries); '
          f'{native.ROOT_LINKS["glibc-shim"]} -> {root} (was {previous or "absent"})')


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('action', choices=('check', 'stage'))
    ap.add_argument('--bundle', required=True, type=Path)
    ap.add_argument('--root', type=Path, default=Path(native.GLIBC_SHIM_STAGING))
    ap.add_argument('--native-gl', choices=('on', 'off'), default='off')
    ap.add_argument('--tag')
    a = ap.parse_args(argv)
    try:
        if a.action == 'check':
            check(a.bundle, a.native_gl == 'on')
        else:
            stage(a.bundle, a.root, a.native_gl == 'on', a.tag)
    except (Refused, OSError) as error:
        sys.exit(f'glibc-shim: {error}')


if __name__ == '__main__':
    main(sys.argv[1:])
