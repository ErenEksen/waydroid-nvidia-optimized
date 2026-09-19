import json
from pathlib import Path
import sys
import tempfile
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[2]/'dev'))
from perf.runtime import resource_summary


class ResourceTests(unittest.TestCase):
    def test_rates_and_missing_gpu(self):
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp)
            rows=[{'monotonic_ns':1, 'cpu.stat':'usage_usec 100000', 'memory.current':'4096',
                   'processes':[{'fds':6,'threads':'2'}]},
                  {'monotonic_ns':250000001,'cpu.stat':'usage_usec 200000','memory.current':'8192',
                   'processes':[{'fds':6,'threads':'2'}]}]
            (p/'resources.jsonl').write_text('\n'.join(json.dumps(r) for r in rows))
            (p/'gpu.csv').write_text('timestamp, utilization.gpu [%]\nnow, 0 %\n')
            result=resource_summary(p)
            self.assertTrue(result['complete'])
            self.assertEqual(result['cpu_core_percent']['mean'],40)
            self.assertEqual(result['gpu_percent']['mean'],0)
            (p/'gpu.csv').write_text('timestamp, utilization.gpu [%]\nnow, [N/A]\n')
            self.assertFalse(resource_summary(p)['complete'])
