"""patches/virglrenderer/0004: vkr_context_import_semaphore_sync_fd() built from the patch text and run against a
fake object table and driver (tests/host/virgl_import_harness.c)."""
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

PATCH = REPO / 'patches/virglrenderer/0004-wip-gpu-alloc-and-global-priority.patch'


def snippet():
    src = patchsrc.post_image(PATCH, 'src/venus/vkr_context.c')
    return 'static int\n' + patchsrc.c_block(src, 'vkr_context_import_semaphore_sync_fd(struct vkr_context *ctx,')


@unittest.skipUnless(shutil.which('cc') and Path('/usr/include/vulkan/vulkan.h').exists(), 'needs cc and Vulkan headers')
class VirglImport(unittest.TestCase):
    def test_harness(self):
        with tempfile.TemporaryDirectory(prefix='nvwd-virgl-') as tmp:
            Path(tmp, 'snippet.c').write_text(snippet())
            subprocess.run(['cc', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-I', tmp,
                            '-o', f'{tmp}/harness', str(HERE / 'virgl_import_harness.c'), '-lpthread'], check=True)
            out = subprocess.run([f'{tmp}/harness'], capture_output=True, text=True, timeout=60)
            self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
            self.assertIn('all checks passed', out.stdout)


if __name__ == '__main__':
    unittest.main()
