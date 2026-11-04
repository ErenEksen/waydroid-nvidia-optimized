"""patches/mesa/0003: the vtest fence export and semaphore import code, built from the patch text and run against
a fake kernel and socket (tests/host/mesa_vtest_harness.c)."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(HERE))
import patchsrc  # noqa: E402

PATCH = REPO / 'patches/mesa/0003-wip-ahb-memory-steering.patch'
FILE = 'src/virtio/vulkan/vn_renderer_vtest.c'


def snippet():
    src = patchsrc.post_image(PATCH, FILE)
    return (patchsrc.c_lines(src, 'struct vtest_syncobj_handle_point {',
                             '   DRM_IOWR(0xC1, struct vtest_syncobj_handle_point)')
            + patchsrc.c_block(src, 'vtest_timeline_export_sync_file(struct vtest *vtest, uint64_t point)')
            .join(['static int\n', ''])
            + patchsrc.c_block(src, 'vtest_semaphore_import_sync_fd(struct vn_renderer *renderer,')
            .join(['static VkResult\n', '']))


@unittest.skipUnless(shutil.which('cc') and Path('/usr/include/libdrm/drm.h').exists() and
                     Path('/usr/include/vulkan/vulkan.h').exists(), 'needs cc, libdrm and Vulkan headers')
class MesaVtest(unittest.TestCase):
    def test_harness(self):
        with tempfile.TemporaryDirectory(prefix='nvwd-mesa-') as tmp:
            Path(tmp, 'snippet.c').write_text(snippet())
            exe = Path(tmp, 'harness')
            subprocess.run(['cc', '-std=gnu11', '-O1', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                            '-I', tmp, '-o', str(exe), str(HERE / 'mesa_vtest_harness.c'), '-lpthread'], check=True)
            out = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
            self.assertIn('all checks passed', out.stdout)


if __name__ == '__main__':
    unittest.main()
