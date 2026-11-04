#!/usr/bin/env python3
"""Check a built waydroid-nvidia-optimized package (.pkg.tar.zst) before anyone installs it.

    packaging/arch/check-package.py PACKAGE [--built-here]

Checks the metadata, that every file the setup and the Venus path need is there with the right ELF ABI (Android
lib/ = ELF32 i386, lib64/ = ELF64 x86-64), the host renderer's RUNPATH, the services' contents, the upstream
release bundle against its own BUILD-RECORD.json, ownership and modes. --built-here also requires the fork's code
in the binaries the PKGBUILD builds (a package assembled from other binaries fails it).
"""
import hashlib
import io
import json
import re
import stat
import struct
import subprocess
import sys
import tarfile
import tempfile

REQUIRED = {
    # path: (kind, ELF class, ELF machine)   kind: f = file, x = executable, l = symlink
    'usr/bin/waydroid': ('l', None, None),
    'usr/lib/waydroid/waydroid.py': ('x', None, None),
    'usr/lib/waydroid/tools/helpers/nvidia_native.py': ('f', None, None),
    'usr/lib/systemd/system/waydroid-container.service': ('f', None, None),
    'usr/share/dbus-1/system.d/id.waydro.Container.conf': ('f', None, None),
    'usr/bin/waydroid-nvidia-setup': ('x', None, None),
    'usr/lib/systemd/user/wd-venus.service': ('f', None, None),
    'usr/lib/tmpfiles.d/waydroid-venus.conf': ('f', None, None),
    'usr/lib/udev/rules.d/70-waydroid-nvidia.rules': ('f', None, None),
    'usr/lib/udev/rules.d/70-waydroid-nvidia-gpu.rules': ('f', None, None),
    'usr/share/libalpm/hooks/90-waydroid-nvidia-driver.hook': ('f', None, None),
    'usr/lib/waydroid-nvidia/driver-update-note': ('x', None, None),
    'usr/lib/waydroid-nvidia/wd-native': ('x', None, None),
    'usr/lib/waydroid-nvidia/BUILD-INFO': ('f', None, None),
    'usr/lib/waydroid-nvidia/virgl_test_server': ('x', 2, 62),
    'usr/lib/waydroid-nvidia/virgl_render_server': ('x', 2, 62),
    'usr/lib/waydroid-nvidia/libvirglrenderer.so.1': ('x', 2, 62),
    'usr/lib/waydroid-nvidia/guest/vendor/lib64/hw/vulkan.virtio.so': ('f', 2, 62),
    'usr/lib/waydroid-nvidia/guest/vendor/lib/hw/vulkan.virtio.so': ('f', 1, 3),
    'usr/lib/waydroid-nvidia/guest/vendor/lib64/libgbm_mesa_wrapper.so': ('f', 2, 62),
    'usr/lib/waydroid-nvidia/guest/vendor/lib64/hw/hwcomposer.waydroid.so': ('f', 2, 62),
    'usr/lib/waydroid-nvidia/guest/vendor/lib64/egl/libEGL_angle.so': ('f', 2, 62),
    'usr/lib/waydroid-nvidia/guest/vendor/lib64/egl/libGLESv1_CM_angle.so': ('f', 2, 62),
    'usr/lib/waydroid-nvidia/guest/vendor/lib64/egl/libGLESv2_angle.so': ('f', 2, 62),
    'usr/lib/waydroid-nvidia/guest/vendor/lib/egl/libEGL_angle.so': ('f', 1, 3),
    'usr/lib/waydroid-nvidia/guest/vendor/lib/egl/libGLESv1_CM_angle.so': ('f', 1, 3),
    'usr/lib/waydroid-nvidia/guest/vendor/lib/egl/libGLESv2_angle.so': ('f', 1, 3),
    'usr/lib/waydroid-nvidia/guest/system/bin/surfaceflinger': ('x', 2, 62),
    'usr/lib/waydroid-nvidia/native/PACKAGED': ('f', None, None),
    'usr/lib/waydroid-nvidia/native/release/BUILD-RECORD.json': ('f', None, None),
    'usr/lib/waydroid-nvidia/native/dev/source-native': ('x', None, None),
    'usr/lib/waydroid-nvidia/native/packaging/native/release_gate.py': ('f', None, None),
}
VENDOR = re.compile(r'^usr/lib/waydroid-nvidia/vendor/waydroid-nvidia-vendor-(x86_64-v[^/]+\.zip|ota-v[^/]+\.json)$')
ALLOWED_TOP = ('usr/', 'etc/xdg/')
SONAMES = {
    'usr/lib/waydroid-nvidia/guest/vendor/lib64/hw/vulkan.virtio.so': 'libvulkan_virtio.so',
    'usr/lib/waydroid-nvidia/guest/vendor/lib/hw/vulkan.virtio.so': 'libvulkan_virtio.so',
    'usr/lib/waydroid-nvidia/guest/vendor/lib64/libgbm_mesa_wrapper.so': 'libgbm_mesa_wrapper.so',
}
# strings only this fork's sources put in the binaries the PKGBUILD builds
FORK_MARKERS = {
    'usr/lib/waydroid-nvidia/guest/vendor/lib64/hw/vulkan.virtio.so': [b'timeline export failed, socket fallback'],
    'usr/lib/waydroid-nvidia/guest/vendor/lib/hw/vulkan.virtio.so': [b'timeline export failed, socket fallback'],
    'usr/lib/waydroid-nvidia/virgl_test_server': [b'vtest_gpu_alloc: allocating on',
                                                  b'VCMD_RESOURCE_ALLOC_GPU rate limit hit'],
    'usr/lib/waydroid-nvidia/libvirglrenderer.so.1': [b'import_semaphore: bad object ids',
                                                      b'refusing to signal incomplete GPU work'],
}

failures = []


def fail(msg):
    failures.append(msg)
    print(f'FAIL {msg}')


def elf_header(blob):
    if len(blob) < 64 or blob[:4] != b'\x7fELF':
        return None
    return blob[4], struct.unpack_from('<H', blob, 18)[0]


def readelf(blob, *args):
    # readelf seeks: give it a file, not a pipe
    with tempfile.NamedTemporaryFile(prefix='nvwd-check-') as f:
        f.write(blob)
        f.flush()
        return subprocess.run(['readelf', *args, f.name], capture_output=True, check=True,
                              env={'LC_ALL': 'C', 'PATH': '/usr/bin:/bin'}).stdout.decode()


def main():
    if len(sys.argv) not in (2, 3) or (len(sys.argv) == 3 and sys.argv[2] != '--built-here'):
        raise SystemExit(__doc__)
    built_here = len(sys.argv) == 3
    raw = subprocess.run(['zstd', '-dc', sys.argv[1]], capture_output=True, check=True).stdout
    members, data = {}, {}
    with tarfile.open(fileobj=io.BytesIO(raw)) as tar:
        for m in tar.getmembers():
            members[m.name] = m
            if m.isfile():
                data[m.name] = tar.extractfile(m).read()

    info = {}
    for line in data.get('.PKGINFO', b'').decode().splitlines():
        if ' = ' in line:
            k, v = line.split(' = ', 1)
            info.setdefault(k, []).append(v)
    for key, want in (('pkgname', 'waydroid-nvidia-optimized'), ('arch', 'x86_64')):
        if info.get(key) != [want]:
            fail(f'.PKGINFO {key} = {info.get(key)}')
    for want in ('waydroid',):
        if want not in info.get('provides', []) or want not in info.get('conflict', []):
            fail(f'.PKGINFO must provide and conflict with {want}')
    for dep in ('lxc', 'python-gbinder>=1.3.0', 'vulkan-icd-loader', 'libepoxy'):
        if dep not in info.get('depend', []):
            fail(f'.PKGINFO lacks depend {dep}')
    if '.INSTALL' not in data:
        fail('no .INSTALL scriptlet')

    for path, (kind, cls, mach) in REQUIRED.items():
        m = members.get(path)
        if m is None:
            fail(f'missing {path}')
            continue
        if kind == 'l':
            if not m.issym():
                fail(f'{path} is not a symlink')
            continue
        if not m.isfile():
            fail(f'{path} is not a regular file')
            continue
        if kind == 'x' and not m.mode & 0o111:
            fail(f'{path} is not executable ({oct(m.mode)})')
        if cls:
            h = elf_header(data[path])
            if h != (cls, mach):
                fail(f'{path}: ELF class/machine {h}, want {(cls, mach)}')
    if members.get('usr/bin/waydroid') is not None and members['usr/bin/waydroid'].linkname != '../lib/waydroid/waydroid.py':
        fail(f'usr/bin/waydroid -> {members["usr/bin/waydroid"].linkname}')
    vendor = [n for n in members if VENDOR.match(n)]
    if len(vendor) != 2:
        fail(f'vendor image zip + channel JSON expected, found {vendor}')

    for path, soname in SONAMES.items():
        if path in data and f'Library soname: [{soname}]' not in readelf(data[path], '-dW'):
            fail(f'{path}: SONAME is not {soname}')
    for path in ('usr/lib/waydroid-nvidia/virgl_test_server', 'usr/lib/waydroid-nvidia/virgl_render_server',
                 'usr/lib/waydroid-nvidia/libvirglrenderer.so.1'):
        if path in data:
            dyn = readelf(data[path], '-dW')
            runpath = re.findall(r'\((?:RUNPATH|RPATH)\).*\[(.*)\]', dyn)
            if runpath != ['$ORIGIN']:
                fail(f'{path}: RUNPATH {runpath}, want [$ORIGIN]')
    if built_here:
        for path, markers in FORK_MARKERS.items():
            for marker in markers:
                if marker not in data.get(path, b''):
                    fail(f'{path} lacks this fork\'s code ({marker.decode()!r})')

    svc = data.get('usr/lib/systemd/user/wd-venus.service', b'').decode()
    for want in ('ExecStart=/usr/lib/waydroid-nvidia/virgl_test_server --venus', 'LD_LIBRARY_PATH=/usr/lib/waydroid-nvidia',
                 'RENDER_SERVER_EXEC_PATH=/usr/lib/waydroid-nvidia/virgl_render_server',
                 '__GL_SHADER_DISK_CACHE_PATH=%C/waydroid-nvidia/nvidia', '__GL_SHADER_DISK_CACHE_SIZE='):
        if want not in svc:
            fail(f'wd-venus.service lacks {want}')

    rec_path = 'usr/lib/waydroid-nvidia/native/release/BUILD-RECORD.json'
    if rec_path in data:
        record = json.loads(data[rec_path])
        base = 'usr/lib/waydroid-nvidia/native/release/'
        files = {**record.get('files', {}), **(record.get('glibc_shim_files') or {})}
        bad = [r for r, digest in files.items() if hashlib.sha256(data.get(base + r, b'')).hexdigest() != digest]
        if bad:
            fail(f'{len(bad)} release bundle files differ from BUILD-RECORD.json (first {bad[0]})')
        build_info = dict(l.split('=', 1) for l in data.get('usr/lib/waydroid-nvidia/BUILD-INFO', b'').decode().split()
                          if '=' in l)
        if build_info.get('upstream_release') and build_info['upstream_release'] != record.get('release'):
            fail(f'BUILD-INFO upstream_release {build_info["upstream_release"]} != bundle {record.get("release")}')

    for name, m in members.items():
        if name.startswith('.'):
            continue
        if not name.startswith(ALLOWED_TOP) and not (m.isdir() and any(t.startswith(name + '/') for t in ALLOWED_TOP)):
            fail(f'file outside /usr and /etc/xdg: {name}')
        if m.uid != 0 or m.gid != 0:
            fail(f'{name} not owned by root ({m.uid}:{m.gid})')
        if not m.issym() and m.mode & stat.S_IWOTH:
            fail(f'{name} is world-writable')

    n = sum(1 for k in members if not k.startswith('.'))
    print(f'{sys.argv[1]}: {n} entries, {len(failures)} failure(s)')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
