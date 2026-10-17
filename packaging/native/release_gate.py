#!/usr/bin/env python3
"""Install-time gate of the released nvidia-prolapse payload (no compiler, no checkout build tree).

The developer stage (dev/source-native stage) builds and runs its gate programs from build/. A user
installs the CI-built release bundle instead (packaging/native/collect.py):

  BUNDLE/BUILD-RECORD.json   sha256 of every payload/ and gate/ file, commit, Mesa pin, Rust builds
  BUNDLE/payload/            the staged-root layout (vendor_extra/lib{,64}, vendor/lib{,64}/egl, odm/bin)
  BUNDLE/gate/               CI-built gate programs, SPIR-V and llvmpipe references (build-gate.py)

and `dev/source-native stage-release BUNDLE NEWROOT` (run by waydroid-nvidia-setup) calls run() here:

  1. every bundle file is hash-checked against BUILD-RECORD.json (a modified install is refused);
  2. the bionic runtime the programs need (linker, libc/libm/libdl, the DT_NEEDED closure that the
     payload does not provide) is extracted read-only from the user's own system.img, per ABI;
  3. the cases of the developer gate run on the user's GPU and loaded kernel module against the exact
     payload bytes: Vulkan compute/graphics/interop + HAL ANB/ASTC (x86_64 and i686), nvrm GL smoke
     (triangle vs llvmpipe reference, dma-buf/fence capability, Skia shaders, YUV import), Codec2 caps
     (nvc2-xmlgen -> stage-odm.sh -> --check, ELF closure against the images, device access);
  4. logs stay in LOG_ROOT/gate-<time>/ (default /var/lib/waydroid-nvidia).

Any failure raises RuntimeError naming the failing case, its log and the remedy; nothing is staged.
The records returned have the shape dev/source-native's manifest (format 3) requires, with
"runner" naming this packaged gate.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import time

REPO = Path(__file__).resolve().parents[2]
BUNDLE = Path('/usr/lib/waydroid-nvidia/native/release')
LOG_ROOT = Path('/var/lib/waydroid-nvidia')
RECORD = 'BUILD-RECORD.json'
RECORD_FORMAT = 2
ABIS = {'android': ('lib64', 64, 'x86_64'), 'android32': ('lib', 32, 'x86')}
# libraries the developer gate's 32-bit runtime closure starts from (prepare-runtime32.py): dlopen()ed
# by the HAL/EGL paths, so they are not all reachable through DT_NEEDED
RUNTIME_ROOTS = ('libcutils.so', 'libhardware.so', 'liblog.so', 'libsync.so', 'libnativewindow.so')
BIONIC = ('libc.so', 'libm.so', 'libdl.so', 'libdl_android.so')
CASE_TIMEOUT = 600
REMEDY_BUNDLE = 'reinstall the package (Arch: pacman -S waydroid-nvidia-bin; other distros: packaging/install.sh)'


def sha256(path):
    with open(path, 'rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def now():
    import datetime
    return datetime.datetime.now().astimezone().isoformat(timespec='seconds')


def bundle_files(bundle):
    """{relative path: path} of every file under payload/ and gate/."""
    files = {}
    for top in ('payload', 'gate'):
        base = bundle / top
        for path in sorted(base.rglob('*')):
            if path.is_file() or path.is_symlink():
                files[str(path.relative_to(bundle))] = path
    return files


def verify_bundle(bundle=BUNDLE):
    """The parsed BUILD-RECORD.json after checking that the bundle holds exactly its files, unmodified."""
    bundle = Path(bundle)
    try:
        record = json.loads((bundle / RECORD).read_text())
    except (OSError, ValueError) as error:
        raise RuntimeError(f'release bundle {bundle} unreadable ({error}); {REMEDY_BUNDLE}')
    if record.get('format') != RECORD_FORMAT or not isinstance(record.get('files'), dict):
        raise RuntimeError(f'{bundle / RECORD} is format {record.get("format")!r}, this gate reads format '
                           f'{RECORD_FORMAT}; install matching waydroid-nvidia tools and release assets')
    have = bundle_files(bundle)
    want = record['files']
    problems = [f'missing {r}' for r in sorted(set(want) - set(have))]
    problems += [f'not in the build record: {r}' for r in sorted(set(have) - set(want))]
    problems += [f'modified: {r}' for r in sorted(set(want) & set(have))
                 if have[r].is_symlink() or sha256(have[r]) != want[r]]
    if problems:
        raise RuntimeError(f'release bundle {bundle} does not match its build record ({len(problems)} problems; '
                           f'first: {problems[0]}); {REMEDY_BUNDLE}')
    return record


# ---------------------------------------------------------------- bionic runtime from the user's image

def elf_bits(path):
    try:
        with open(path, 'rb') as f:
            head = f.read(5)
    except OSError:
        return None
    if head[:4] != b'\x7fELF':
        return None
    return {1: 32, 2: 64}.get(head[4])


def needed(path):
    dynamic = subprocess.check_output(['readelf', '-dW', str(path)], text=True)
    return re.findall(r'\(NEEDED\)\s+Shared library: \[(.+?)\]', dynamic)


def _dump(image, source, dest):
    dest.unlink(missing_ok=True)
    subprocess.run(['debugfs', '-R', f'dump {source} {dest}', str(image)], capture_output=True, check=True)
    return dest.is_file() and dest.stat().st_size > 0


def extract_runtime(system_img, dest, sub, roots, provided):
    """Copy the image's linker and the DT_NEEDED closure of `roots` (names) that `provided` (names the
    payload ships) does not cover into dest. Returns {name: image path}."""
    bits = 64 if sub == 'lib64' else 32
    dest.mkdir(parents=True, exist_ok=True)
    origin = {}
    linker = 'linker64' if bits == 64 else 'linker'
    if not _dump(system_img, f'/system/apex/com.android.runtime/bin/{linker}', dest / linker) or \
            elf_bits(dest / linker) != bits:
        raise RuntimeError(f'{system_img} has no ELF{bits} /system/apex/com.android.runtime/bin/{linker}; the gate '
                           'runs its programs with the image\'s own bionic linker (is this a Waydroid x86_64 image?)')
    (dest / linker).chmod(0o755)
    origin[linker] = f'/system/apex/com.android.runtime/bin/{linker}'
    pending, seen = list(roots), set(provided)
    while pending:
        name = pending.pop()
        if name in seen:
            continue
        seen.add(name)
        if '/' in name or name in ('.', '..'):
            raise RuntimeError(f'invalid DT_NEEDED name {name!r}')
        candidates = [f'/system/{sub}/{name}', f'/system/apex/com.android.vndk.v33/{sub}/{name}',
                      f'/system/apex/com.android.vndk.current/{sub}/{name}']
        if name in BIONIC:
            candidates.insert(0, f'/system/apex/com.android.runtime/{sub}/bionic/{name}')
        for source in candidates:
            if _dump(system_img, source, dest / name) and elf_bits(dest / name) == bits:
                origin[name] = source
                pending += needed(dest / name)
                break
        else:
            (dest / name).unlink(missing_ok=True)
            raise RuntimeError(f'{system_img} provides no ELF{bits} {name} (looked in {", ".join(candidates)}); '
                               'the release payload needs it. Is /var/lib/waydroid/images the official LineageOS 20 '
                               'x86_64 image? (waydroid init -f)')
    return origin


# ---------------------------------------------------------------- cases

class Gate:
    def __init__(self, bundle, work, system_img):
        self.bundle, self.work, self.system_img = Path(bundle), Path(work), Path(system_img)
        self.payload, self.progs = self.bundle / 'payload', self.bundle / 'gate'
        self.runtime, self.libdirs = {}, {}
        self.cases = []

    def libpath(self, target):
        sub = ABIS[target][0]
        return [self.payload / 'vendor_extra' / sub, self.payload / 'vendor' / sub / 'egl', self.runtime[target]]

    def prepare(self, target):
        sub, bits, _ = ABIS[target]
        dirs = [self.payload / 'vendor_extra' / sub, self.payload / 'vendor' / sub / 'egl']
        provided = {p.name for d in dirs for p in d.iterdir() if p.is_file()}
        # what the cases load: the programs, the ICD and HAL they are given, nvrm (by SONAME from the
        # programs), followed through DT_NEEDED inside the payload; names the payload lacks come from the image
        roots = set(RUNTIME_ROOTS)
        todo = [*(self.progs / 'bin' / target).iterdir(), self.payload / 'vendor_extra' / sub / 'libvulkan_nouveau.so',
                self.payload / 'vendor_extra' / sub / 'hw/vulkan.nvidia.so']
        seen = set()
        while todo:
            obj = todo.pop()
            if obj in seen:
                continue
            seen.add(obj)
            for name in needed(obj):
                local = next((d / name for d in dirs if (d / name).is_file()), None)
                if local is not None:
                    todo.append(local)
                else:
                    roots.add(name)
        out = self.work / 'runtime' / sub
        origin = extract_runtime(self.system_img, out, sub, sorted(roots), provided)
        self.runtime[target] = out
        return origin

    def command(self, target, argv):
        sub, bits, _ = ABIS[target]
        linker = self.runtime[target] / ('linker64' if bits == 64 else 'linker')
        env = {'LD_LIBRARY_PATH': ':'.join(map(str, self.libpath(target))), 'DISPLAY': '', 'WAYLAND_DISPLAY': ''}
        command = [str(linker), *map(str, argv)]
        if bits == 32:
            # i686 bionic's pthread TID field cannot hold large host PIDs: run in a fresh PID namespace
            command = ['unshare', '--user', '--map-root-user', '--pid', '--fork', *command]
        return env, command

    def run(self, target, name, argv, env_extra=None, markers=(), check_exit=True, judge=None):
        env, command = self.command(target, argv)
        env.update(env_extra or {})
        log = self.work / 'logs' / f'{target}-{name}.log'
        log.parent.mkdir(parents=True, exist_ok=True)
        print(f'[{target}/{name}]', flush=True)
        t0 = time.monotonic()
        try:
            proc = subprocess.run(command, env={**os.environ, **env}, stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, text=True, timeout=CASE_TIMEOUT, cwd=self.work)
            output, code = proc.stdout, proc.returncode
        except subprocess.TimeoutExpired as error:
            output, code = (error.stdout or b'').decode(errors='replace') if isinstance(error.stdout, bytes) \
                else (error.stdout or ''), 'timeout'
        output = ''.join(l for l in output.splitlines(True) if 'linker configuration' not in l)
        log.write_text(' '.join(f'{k}={v}' for k, v in env.items()) + ' ' + ' '.join(command) + '\n' + output)
        seconds = round(time.monotonic() - t0, 2)
        failed = (check_exit and code != 0) or code == 'timeout' or any(m not in output for m in markers)
        if judge and not failed:
            failed = not judge(output)
        verdict = 'FAIL' if failed else 'PASS'
        how = '' if check_exit else ' (judged by its output, not the exit code)'
        print(f'{verdict} exit={code} {seconds}s{how} log={log}', flush=True)
        self.cases.append({'case': f'{target}/{name}', 'exit': 0 if not failed else (code if code else 1),
                           'seconds': seconds, 'log': str(log)})
        if failed:
            tail = '\n'.join(output.splitlines()[-25:])
            raise RuntimeError(f'gate case {target}/{name} FAILED (exit {code}); log {log}\n{tail}')
        return output

    def exercised(self, targets):
        """payload-relative paths reached by the cases: ICD/HAL arguments and their DT_NEEDED closure
        inside the payload (dlopen()ed libraries are not followed: a lower bound, as in the dev gate)."""
        seen = set()
        for target in targets:
            sub = ABIS[target][0]
            todo = [self.payload / 'vendor_extra' / sub / 'libvulkan_nouveau.so',
                    self.payload / 'vendor_extra' / sub / 'hw/vulkan.nvidia.so']
            todo += [p for p in (self.progs / 'bin' / target).iterdir()]
            dirs = [self.payload / 'vendor_extra' / sub, self.payload / 'vendor' / sub / 'egl']
            while todo:
                path = todo.pop()
                if path in seen:
                    continue
                seen.add(path)
                for name in needed(path):
                    for d in dirs:
                        if (d / name).is_file():
                            todo.append(d / name)
                            break
        return sorted(str(p.relative_to(self.payload)) for p in seen if self.payload in p.parents)


def vulkan_cases(gate, target):
    sub = ABIS[target][0]
    icd = gate.payload / 'vendor_extra' / sub / 'libvulkan_nouveau.so'
    hal = gate.payload / 'vendor_extra' / sub / 'hw/vulkan.nvidia.so'
    b, d = gate.progs / 'bin' / target, gate.progs / 'data'
    gate.run(target, 'compute', [b / 'vk-compute', '--icd', '--driver', icd, '--shader', d / 'generate.spv',
                                 '--transform', d / 'transform.spv', '--negative'])
    gate.run(target, 'graphics', [b / 'vk-graphics', icd, d / 'vertex.spv', d / 'fragment.spv'])
    gate.run(target, 'interop', [b / 'vk-interop', '--driver', icd])
    gate.run(target, 'anb', [b / 'anb-roundtrip', hal, '--bounded'])
    gate.run(target, 'astc', [b / 'astc-roundtrip', hal])
    gate.run(target, 'etc2', [b / 'etc2-roundtrip', hal])


def gl_cases(gate, target):
    sub = ABIS[target][0]
    b, ref = gate.progs / 'bin' / target, gate.progs / 'ref'
    out = gate.work / 'gl' / target
    (out / 'skia').mkdir(parents=True, exist_ok=True)
    gate.run(target, 'gl-flat', [b / 'gl-triangle', '--es3', '--flat', 'nvrm', out / 'flat.rgba', ref / 'ref-flat.rgba'])

    def gradient(log):   # <= 1 LSB interpolation differences to llvmpipe are expected (gl-android-smoke.py)
        maxd = re.search(r'max channel difference (\d+)', log)
        return 'OpenGL ES 3.' in log and 'coverage differences 0,' in log and maxd and int(maxd.group(1)) <= 1
    gate.run(target, 'gl-gradient', [b / 'gl-triangle', '--es3', 'nvrm', out / 'grad.rgba', ref / 'ref-grad.rgba'],
             check_exit=False, judge=gradient)
    gate.run(target, 'gl-capability', [b / 'gl-dmabuf', 'nvrm'], markers=('GL DMABUF PASS',))
    gate.run(target, 'gl-skia-shaders', [b / 'gl-skia-shaders', 'nvrm', out / 'skia', ref / 'skia'],
             markers=('SKIA SHADERS PASS',))
    gate.run(target, 'gl-yuv', [b / 'gl-yuv', 'nvrm'], markers=('GL YUV PASS',),
             env_extra={'NVRM_YUV_VK_ICD': str(gate.payload / 'vendor_extra' / sub / 'libvulkan_nouveau.so')})


def media_case(gate, sn, native, vendor_img):
    """Codec2 NVDEC: caps xml generated on this GPU through the shipped backend, the /odm tree, the ELF
    closure against the user's images, device access. Returns (media record, {odm relative: path})."""
    env, command = gate.command('android', [gate.progs / 'bin/android/nvc2-xmlgen'])
    wrapper = gate.work / 'nvc2-xmlgen'
    wrapper.write_text('#!/bin/sh\nexec env ' + ' '.join(f"{k}='{v}'" for k, v in env.items()) + ' ' +
                       ' '.join(f"'{c}'" for c in command) + ' "$@"\n')
    wrapper.chmod(0o755)
    service = gate.payload / native.MEDIA_SERVICE
    record = {'exit': None, 'kernel_module': native.driver_version(), 'rust_profile': 'release', 'started': now(),
              'runner': 'packaging/native/release_gate.py (packaged media gate)', 'steps': []}
    workdir = gate.work / 'media'
    odm = workdir / 'odm'
    workdir.mkdir()
    log = gate.work / 'logs' / 'media.log'

    def step(name, *cmd):
        print(f'[media/{name}]', flush=True)
        t0 = time.monotonic()
        proc = subprocess.run(list(map(str, cmd)), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                              cwd=REPO)
        with open(log, 'a') as f:
            f.write(f'$ {" ".join(map(str, cmd))}\n{proc.stdout}exit={proc.returncode}\n\n')
        record['steps'].append({'step': name, 'exit': proc.returncode, 'seconds': round(time.monotonic() - t0, 2)})
        if proc.returncode != 0:
            raise RuntimeError(f'media gate FAILED at {name} (exit {proc.returncode}); log {log}\n' + proc.stdout[-3000:])
    record['steps'].append({'step': 'host-tests', 'exit': 0, 'note': 'run at build time (CI: make -C nvidia-prolapse/media test)'})
    step('stage-odm', 'bash', REPO / 'nvidia-prolapse/media/stage-odm.sh', odm, vendor_img, service, wrapper)
    xml = workdir / native.MEDIA_XML
    step('xmlgen-check', wrapper, '--check', xml)
    want = set(native.MEDIA_ODM_FILES) | {'odm/etc/' + n for n in native.ODM_ORIGINALS}
    have = {str(p.relative_to(workdir)) for p in odm.rglob('*') if p.is_file() or p.is_symlink()}
    if have != want:
        raise RuntimeError(f'stage-odm.sh produced {sorted(have)}, expected {sorted(want)}')
    if sha256(workdir / native.MEDIA_SERVICE) != sha256(service):
        raise RuntimeError('stage-odm.sh installed a service binary that differs from the release payload')
    libs = [gate.payload / 'vendor_extra/lib64' / n for n in native.MEDIA_LIBS]
    rust = gate.payload / 'vendor_extra/lib64/libnv_bionic_native.so'
    closure, symbols = sn.media_closure(service, libs, rust, gate.system_img, vendor_img)
    native.check_media_device_access([native.DEV_DIR + '/nvidiactl'] +
                                     sorted(str(p) for p in Path(native.DEV_DIR).glob('nvidia[0-9]*')) +
                                     [native.nvidia_render_node()])
    record.update(exit=0, finished=now(), log=str(log), closure=closure, nv_video_symbols=symbols,
                  android_backend={'path': str(rust), 'sha256': sha256(rust)})
    media = {'enabled': True, 'component': 'nvidia-prolapse/media Codec2 NVDEC decoder',
             'vendor_img': str(vendor_img), 'system_img': str(gate.system_img),
             'codecs': sn.media_codecs(xml), 'gate': record}
    return media, {str(p.relative_to(workdir)): p for p in sorted(odm.rglob('*')) if p.is_file()}


VNDK_APEX = '/system/apex/com.android.vndk.current/lib64'
LLNDK = ('libc.so', 'libm.so', 'libdl.so', 'liblog.so', 'libnativewindow.so', 'libsync.so', 'libvndksupport.so',
         'libvulkan.so', 'libandroid_net.so', 'libbinder_ndk.so', 'libmediandk.so', 'libselinux.so', 'libEGL.so',
         'libGLESv1_CM.so', 'libGLESv2.so', 'libGLESv3.so', 'libft2.so')


def media_case_image(gate, native, vendor_img):
    """Codec2 NVDEC of the nvidia-prolapse VENDOR IMAGE (its /odm service and /lib64 HIDL/Codec2 libraries are in
    vendor_img): this GPU's codec list generated through the shipped backend (nvc2-xmlgen, validate_xml.py,
    --check against the live caps), the service's ELF closure against vendor_img + the system image's VNDK apex
    and LL-NDK, the nv_video symbols it imports, device access for uid mediacodec. Returns the media record;
    its 'xml' is the generated file (the setup installs it and the LXC generator binds it over the image's
    placeholder /odm/etc/media_codecs_c2.xml)."""
    env, command = gate.command('android', [gate.progs / 'bin/android/nvc2-xmlgen'])
    record = {'exit': None, 'kernel_module': native.driver_version(), 'started': now(),
              'runner': 'packaging/native/release_gate.py (vendor image media gate)', 'steps': []}
    workdir = gate.work / 'media'
    workdir.mkdir()
    xml = workdir / 'media_codecs_c2.xml'
    log = gate.work / 'logs' / 'media.log'

    def step(name, cmd, env_extra=None):
        print(f'[media/{name}]', flush=True)
        t0 = time.monotonic()
        proc = subprocess.run(list(map(str, cmd)), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                              env={**os.environ, **(env_extra or {})}, cwd=gate.work)
        with open(log, 'a') as f:
            f.write(f'$ {" ".join(map(str, cmd))}\n{proc.stdout}exit={proc.returncode}\n\n')
        record['steps'].append({'step': name, 'exit': proc.returncode, 'seconds': round(time.monotonic() - t0, 2)})
        if proc.returncode != 0:
            raise RuntimeError(f'media gate FAILED at {name} (exit {proc.returncode}); log {log}\n' + proc.stdout[-3000:])
    step('xmlgen', [*command, '-o', xml], env)
    step('validate-xml', ['python3', '-I', REPO / 'nvidia-prolapse/media/test/validate_xml.py', xml])
    step('xmlgen-check', [*command, '--check', xml], env)
    if native.XMLGEN_MARK not in xml.read_text():
        raise RuntimeError(f'{xml} lacks the "{native.XMLGEN_MARK}" header')
    service = gate.payload / native.MEDIA_SERVICE           # byte-identical to the image's (checked by the caller)
    vndk = set(native.image_ls(str(gate.system_img), VNDK_APEX))
    system = set(native.image_ls(str(gate.system_img), '/system/lib64'))
    vendor = set(native.image_ls(str(vendor_img), '/lib64'))
    closure, missing = {}, []
    for name in needed(service):
        where = ('vendor.img /lib64' if name in vendor else 'vndk apex' if name in vndk else
                 'llndk /system/lib64' if name in LLNDK and name in system else None)
        if where is None:
            missing.append(name)
        closure[name] = where
    if missing:
        raise RuntimeError(f'the Codec2 service NEEDS {missing}, which neither {vendor_img} /lib64 nor the system '
                           'image provides (vendor image built without them?)')
    rust = gate.payload / 'vendor_extra/lib64/libnv_bionic_native.so'
    nm = lambda path, flag: {l.split()[-1] for l in subprocess.check_output(['nm', '-D', flag, str(path)], text=True)
                             .splitlines() if l.strip()}
    want = {x for x in nm(service, '--undefined-only') if x.startswith('nv_video_')}
    have = {x for x in nm(rust, '--defined-only') if x.startswith('nv_video_')}
    if not want or want - have:
        raise RuntimeError(f'{rust} lacks nv_video symbols the Codec2 service imports: {sorted(want - have) or "none imported"}')
    native.check_media_device_access([native.DEV_DIR + '/nvidiactl'] +
                                     sorted(str(p) for p in Path(native.DEV_DIR).glob('nvidia[0-9]*')) +
                                     [native.nvidia_render_node()])
    record.update(exit=0, finished=now(), log=str(log), closure={service.name: closure}, nv_video_symbols=sorted(want))
    return {'enabled': True, 'component': 'nvidia-prolapse/media Codec2 NVDEC decoder (vendor image)',
            'xml': str(xml), 'xml_sha256': sha256(xml), 'codecs': media_codecs(xml), 'gate': record}


def media_codecs(xml):
    import xml.etree.ElementTree as ET
    return sorted(mc.get('name') for mc in ET.parse(xml).getroot().iter('MediaCodec'))


def run(sn, native, bundle=BUNDLE, images=Path('/var/lib/waydroid/images'), log_root=LOG_ROOT, vendor_image=None):
    """Gate the bundle on this machine. Returns (build record, gate record, media record, native_gl record,
    {staged relative path: source path}) for dev/source-native stage-release. With `vendor_image` (the
    nvidia-prolapse vendor.img about to be installed, whose prebuilts the caller verified byte-identical to
    this bundle's payload) the media case gates that image's Codec2 service instead of staging /odm, and `sn`
    is not used."""
    bundle = Path(bundle)
    record = verify_bundle(bundle)
    system_img = Path(images) / 'system.img'
    vendor_img = Path(vendor_image) if vendor_image else Path(images) / 'vendor.img'
    for img in (system_img, vendor_img):
        if not img.is_file():
            raise RuntimeError(f'{img} missing: run `waydroid init` first')
    kernel = native.driver_version()
    work = Path(log_root) / f'gate-{time.strftime("%Y%m%d-%H%M%S")}'
    work.mkdir(parents=True)
    print(f'Release gate on NVIDIA kernel module {kernel}; logs and runtime in {work}', flush=True)
    gate = Gate(bundle, work, system_img)
    origins = {}
    for target in ABIS:
        origins[target] = gate.prepare(target)
    (work / 'runtime-origin.json').write_text(json.dumps(origins, indent=2) + '\n')
    started, t0 = now(), time.monotonic()
    for target in ABIS:
        vulkan_cases(gate, target)
    vk_cases = list(gate.cases)
    gl_started = now()
    for target in ABIS:
        gl_cases(gate, target)
    gl_cases_run = gate.cases[len(vk_cases):]
    if vendor_image:
        media, odm = media_case_image(gate, native, vendor_img), {}
    else:
        media, odm = media_case(gate, sn, native, vendor_img)
    if native.driver_version() != kernel:
        raise RuntimeError(f'NVIDIA kernel module changed during the gate ({kernel} -> {native.driver_version()})')
    verify_bundle(bundle)    # nothing changed underneath the run
    sources = {rel[len('payload/'):]: p for rel, p in bundle_files(bundle).items() if rel.startswith('payload/')}
    exercised = gate.exercised(ABIS)
    runner = f'packaging/native/release_gate.py (packaged gate, release {record.get("release") or record["commit"][:12]})'
    vk = {'runner': runner, 'rust_profile': 'release', 'runner_sha256': sha256(__file__), 'user': 'root',
          'started': started, 'finished': now(), 'seconds': round(time.monotonic() - t0, 1), 'exit': 0,
          'kernel_module': kernel, 'log_dir': str(work), 'cases': vk_cases, 'exercised': exercised,
          'not_exercised': sorted(set(sources) - set(exercised)),
          'runtime': {t: str(p) for t, p in gate.runtime.items()}}
    gl_files = {rel: sha256(p) for rel, p in sources.items()
                if rel.endswith(tuple(native.NVRM_EGL_LIBS)) or rel.endswith(native.NVRM_GALLIUM)}
    rust_sha = {}
    for target, (sub, _, abi) in ABIS.items():
        staged = sha256(gate.payload / 'vendor_extra' / sub / 'libnv_bionic_native.so')
        linked = record['nvrm_rust_sha256'][abi]
        if linked != staged:
            raise RuntimeError(f'release bundle: nvrm ({abi}) was linked against libnv_bionic_native.so {linked}, the '
                               f'payload ships {staged}; {REMEDY_BUNDLE}')
        rust_sha[abi] = linked
    native_gl = {'enabled': True, 'egl': native.NVRM_EGL, 'files': gl_files,
                 'builds': record.get('nvrm_builds', {}),
                 'gate': {'runner': runner, 'runner_sha256': sha256(__file__), 'started': gl_started,
                          'finished': now(), 'exit': 0, 'kernel_module': kernel, 'log_dir': str(work),
                          'cases': gl_cases_run, 'stripped': 'llvm-strip --strip-debug (CI, collect.py)',
                          'rust_sha256': rust_sha}}
    sources.update(odm)
    print(f'Release gate PASS: {len(gate.cases)} cases + media on kernel module {kernel}', flush=True)
    return record, vk, media, native_gl, sources
