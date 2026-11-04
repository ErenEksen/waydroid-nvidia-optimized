"""src/virglrenderer-vtest/vtest_gpu_alloc.c against a mock Vulkan driver and a fake /dev/udmabuf
(tests/host/gpu_alloc_harness.c): NV12 and RGB sizes, bounds, the per-format modifier cache, cleanup."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
SRC = REPO / 'src/virglrenderer-vtest'


@unittest.skipUnless(shutil.which('cc') and Path('/usr/include/vulkan/vulkan.h').exists(), 'needs cc and Vulkan headers')
class GpuAlloc(unittest.TestCase):
    def test_harness(self):
        with tempfile.TemporaryDirectory(prefix='nvwd-alloc-') as tmp:
            cc = ['cc', '-std=gnu11', '-D_GNU_SOURCE', '-O1', '-g', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Werror',
                  '-U_FORTIFY_SOURCE', '-D_FORTIFY_SOURCE=0']
            subprocess.run(cc + ['-fPIC', '-shared', '-fvisibility=hidden', '-o', f'{tmp}/libvulkan.so.1',
                                 str(HERE / 'mock_vulkan.c')], check=True)
            # -fsanitize: out-of-bounds or use-after-free in the allocator fails the run
            san = ['-fsanitize=address,undefined', '-fno-sanitize-recover=all']
            subprocess.run(cc + san + ['-I', str(SRC), '-o', f'{tmp}/harness', str(HERE / 'gpu_alloc_harness.c'),
                                       str(SRC / 'vtest_gpu_alloc.c'), '-ldl', '-lpthread',
                                       '-Wl,--wrap=open,--wrap=ioctl'], check=True)
            env = dict(os.environ, LD_LIBRARY_PATH=tmp, ASAN_OPTIONS='detect_leaks=1')
            out = subprocess.run([f'{tmp}/harness'], capture_output=True, text=True, env=env)
            self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
            self.assertIn('all checks passed', out.stdout)


if __name__ == '__main__':
    unittest.main()
