import copy
from pathlib import Path
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[2]/'dev'))
from perf.compare import compare
from perf.metrics import focused_bounds, framestats
from perf.presentation import presentation
from test_metrics import block


def report(intervals):
    return {'complete': True, 'schema_version': 2, 'package': 'com.android.settings', 'mode': 'app-cold',
            'requested_runs': 1, 'duration_s': 10, 'desktop_output': 'eDP-1',
            'metadata': {'kernel': 'same', 'gpu': 'same', 'power_profile': 'balanced'},
            'runs': [{'complete': True, 'size': [1920,1200], 'refresh_period_ns': 6060606,
                      'presentation_intervals_ms': intervals,
                      'desktop': {'complete': True, 'missed_source': 'presentation_sequence',
                                  'missed_refreshes': 1, 'expected_refreshes': 100}}]}


class CompareTests(unittest.TestCase):
    def test_cold_gate(self):
        self.assertEqual(compare(report([20,60,80]), report([10,20,40]))['status'], 'pass')
        self.assertEqual(compare(report([20,60,80]), report([10,20,70]))['status'], 'fail')

    def test_partial_cannot_pass(self):
        b=report([20,60,80]); a=report([10,20,40]); a['requested_runs']=10
        self.assertEqual(compare(b,a)['status'], 'incomplete')
        a=report([10,20,40]); a['runs'][0]['desktop']['complete']=False
        self.assertEqual(compare(b,a)['status'], 'incomplete')

    def test_controlled_conditions(self):
        b=report([20,60,80]); a=report([10,20,40]); a['metadata']['power_profile']='performance'
        self.assertEqual(compare(b,a)['status'], 'incomplete')

    def test_desktop_regression(self):
        b=report([20,60,80]); a=report([10,20,40]); a['runs'][0]['desktop']['missed_refreshes']=2
        self.assertEqual(compare(b,a)['status'], 'fail')

    def test_warm_two_periods(self):
        b=report([15]); a=report([12]); b['mode']=a['mode']='warm'
        self.assertEqual(compare(b,a)['status'], 'pass')
        a['runs'][0]['presentation_intervals_ms']=[12.2]
        self.assertEqual(compare(b,a)['status'], 'fail')

    def test_separate_window_streams(self):
        text='Window: a\n'+block(['0,1,1000000,2000000','0,10000001,11000000,12000000'])
        text+='Window: b\n'+block(['0,2,2000000,3000000','0,10000002,12000000,13000000'])
        self.assertEqual(framestats(text)['presentation_intervals_ms'], [10,10])

    def test_idle_gap_is_not_jank_but_active_stall_is(self):
        text=block(['0,1,1000000,2000000','0,100000001,101000000,102000000'])
        self.assertEqual(framestats(text,active_ranges=[[0,103000000]])['presentation_intervals_ms'], [100])
        self.assertEqual(framestats(text,active_ranges=[[0,3000000],[100000000,103000000]])['idle_intervals_excluded'], 1)

    def test_focused_bounds(self):
        text='mCurrentFocus=Window{abc u0 com.android.settings/.Settings}\nWindow #0 Window{abc u0 com.android.settings/.Settings}:\n mFrame=[0,48][1920,1152]'
        self.assertEqual(focused_bounds(text,'com.android.settings'),(0,48,1920,1152))
        with self.assertRaises(ValueError):
            focused_bounds(text,'com.other.app')

    def test_desktop_clock_and_estimate(self):
        header='event,submit_ns,present_ns,refresh_ns,sequence,flags,output_id\n'
        text=header+'presented,1,6000000,6000000,0,7,5\npresented,6000001,18000000,6000000,0,7,5\n'
        result=presentation(text)
        self.assertTrue(result['complete'])
        self.assertEqual(result['missed_source'], 'timestamp_estimate')
        self.assertEqual(result['missed_ratio'], 0.5)
        self.assertFalse(presentation(header)['complete'])
