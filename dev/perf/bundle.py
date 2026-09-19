"""Fixed-target candidate bundle and rollback primitives (no service control)."""
import hashlib
import json
import os
from pathlib import Path
import stat
import struct
import tempfile

# role -> (installed path, ELF class, machine, SONAME). Cache config is user-scoped.
ARTIFACTS = {
    'virgl_test_server': ('usr/lib/waydroid-nvidia/virgl_test_server', 2, 62, None),
    'virgl_render_server': ('usr/lib/waydroid-nvidia/virgl_render_server', 2, 62, None),
    'libvirglrenderer.so.1': ('usr/lib/waydroid-nvidia/libvirglrenderer.so.1', 2, 62, 'libvirglrenderer.so.1'),
    'vulkan-x86_64.so': ('var/lib/waydroid/nv/guest/vendor/lib64/hw/vulkan.virtio.so', 2, 62, 'libvulkan_virtio.so'),
    'vulkan-x86.so': ('var/lib/waydroid/nv/guest/vendor/lib/hw/vulkan.virtio.so', 1, 3, 'libvulkan_virtio.so'),
    'hwcomposer.waydroid.so': ('var/lib/waydroid/nv/guest/vendor/lib64/hw/hwcomposer.waydroid.so', 2, 62, 'hwcomposer.waydroid.so'),
    'libgbm_mesa_wrapper.so': ('var/lib/waydroid/nv/guest/vendor/lib64/libgbm_mesa_wrapper.so', 2, 62, 'libgbm_mesa_wrapper.so'),
    '50-renderer-cache.conf': (None, None, None, None),
}


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def no_symlinks(path):
    for part in (path, *path.parents):
        if part.is_symlink():
            raise ValueError(f'refusing symlink destination/ancestor: {part}')


def destinations(home, root=Path('/')):
    result = {role: root/path for role, (path, *_rest) in ARTIFACTS.items() if path}
    result['50-renderer-cache.conf'] = home/'.config/systemd/user/wd-venus.service.d/50-renderer-cache.conf'
    return result


def load_bundle(directory):
    directory = Path(directory)
    manifest = json.loads((directory/'manifest.json').read_text())
    if manifest.get('schema_version') != 1 or set(manifest.get('files', {})) != set(ARTIFACTS):
        raise ValueError('bundle schema or complete component set mismatch')
    data = {}
    for role, (_, elf_class, machine, _soname) in ARTIFACTS.items():
        path = directory/role
        if path.is_symlink() or not path.is_file():
            raise ValueError(f'not a regular bundle file: {role}')
        blob = path.read_bytes()
        if sha256(blob) != manifest['files'][role]['sha256']:
            raise ValueError(f'bundle checksum mismatch: {role}')
        if elf_class and (len(blob) < 64 or blob[:6] != b'\x7fELF'+bytes([elf_class, 1]) or
                          struct.unpack_from('<H', blob, 18)[0] != machine):
            raise ValueError(f'wrong ELF ABI: {role}')
        data[role] = blob
    return manifest, data


def atomic_write(path, data, mode, uid, gid):
    no_symlinks(path)
    missing = []
    parent = path.parent
    while not parent.exists():
        missing.append(parent)
        parent = parent.parent
    for directory in reversed(missing):
        directory.mkdir(mode=0o755)
        os.chown(directory, uid, gid)
    fd, name = tempfile.mkstemp(prefix='.nvwd-', dir=path.parent)
    try:
        with os.fdopen(fd, 'wb') as out:
            out.write(data)
            out.flush()
            os.fchmod(out.fileno(), mode)
            os.fchown(out.fileno(), uid, gid)
            os.fsync(out.fileno())
        os.replace(name, path)
        dfd = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(dfd)
        finally:
            os.close(dfd)
    finally:
        if os.path.exists(name):
            os.unlink(name)


def backup_set(backup, targets):
    backup.mkdir(mode=0o700, parents=False)
    manifest = {'schema_version': 1, 'files': {}}
    for role, target in targets.items():
        no_symlinks(target)
        if target.exists():
            st = target.stat()
            if not stat.S_ISREG(st.st_mode):
                raise ValueError(f'not a regular installed file: {target}')
            data = target.read_bytes()
            atomic_write(backup/role, data, 0o600, os.getuid(), os.getgid())
            manifest['files'][role] = {'path': str(target), 'sha256': sha256(data),
                'mode': stat.S_IMODE(st.st_mode), 'uid': st.st_uid, 'gid': st.st_gid, 'exists': True}
        else:
            manifest['files'][role] = {'path': str(target), 'exists': False}
    atomic_write(backup/'backup.json', (json.dumps(manifest, indent=2)+'\n').encode(), 0o600, os.getuid(), os.getgid())
    return manifest


def restore_set(backup, targets):
    manifest = json.loads((backup/'backup.json').read_text())
    if manifest.get('schema_version') != 1 or set(manifest['files']) != set(targets):
        raise ValueError('backup role set mismatch')
    data = {}
    # Verify EVERYTHING before modifying any destination.
    for role, target in targets.items():
        entry = manifest['files'][role]
        no_symlinks(target)
        if entry['path'] != str(target):
            raise ValueError('backup target mismatch')
        if entry['exists']:
            data[role] = (backup/role).read_bytes()
            if sha256(data[role]) != entry['sha256']:
                raise ValueError('backup checksum mismatch')
    for role, target in targets.items():
        entry = manifest['files'][role]
        if entry['exists']:
            atomic_write(target, data[role], entry['mode'], entry['uid'], entry['gid'])
        elif target.exists():
            target.unlink()


def install_set(data, targets, uid, gid, system_uid=0, system_gid=0):
    for role, target in targets.items():
        user_file = role.endswith('.conf')
        mode = 0o755 if role in ('virgl_test_server', 'virgl_render_server') else 0o644
        atomic_write(target, data[role], mode, uid if user_file else system_uid, gid if user_file else system_gid)
