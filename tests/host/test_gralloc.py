"""src/minigbm-vtest/vtest_wrapper.c (guest gralloc backend) against a scripted vtest server
(tests/host/gralloc_harness.c). Needs minigbm's gbm_mesa_driver/gbm_mesa_wrapper.h: MINIGBM=<checkout> (the pinned
MINIGBM_SHA of packaging/ci/pins.env), else the test is skipped."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
MINIGBM = Path(os.environ.get('MINIGBM', REPO / '.work/minigbm'))


@unittest.skipUnless(shutil.which('cc') and (MINIGBM / 'gbm_mesa_driver/gbm_mesa_wrapper.h').exists(),
                     'needs cc and MINIGBM=<minigbm checkout>')
class Gralloc(unittest.TestCase):
    def test_harness(self):
        with tempfile.TemporaryDirectory(prefix='nvwd-gralloc-') as tmp:
            subprocess.run(['cc', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Werror',
                            '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                            '-I', str(HERE / 'stubs'), '-I', str(MINIGBM / 'gbm_mesa_driver'),
                            '-o', f'{tmp}/harness', str(HERE / 'gralloc_harness.c'),
                            str(REPO / 'src/minigbm-vtest/vtest_wrapper.c'), '-lpthread'], check=True)
            out = subprocess.run([f'{tmp}/harness', f'{tmp}/venus.sock'], capture_output=True, text=True, timeout=60)
            self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
            self.assertIn('all checks passed', out.stdout)


if __name__ == '__main__':
    unittest.main()
