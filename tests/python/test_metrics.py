import sys
from pathlib import Path
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "dev"))
from perf.metrics import activity_launch, display_size, framestats, percentile, sf_period_ns


def block(rows, present=True):
    header = "Flags,IntendedVsync,FrameCompleted" + (",DisplayPresentTime" if present else "")
    return "---PROFILEDATA---\n" + header + "\n" + "\n".join(rows) + "\n---PROFILEDATA---\n"


class MetricsTests(unittest.TestCase):
    def test_no_samples_is_not_zero(self):
        self.assertIsNone(percentile([], 99))
        self.assertFalse(framestats("")["complete"])

    def test_nearest_rank(self):
        self.assertEqual(percentile([1, 2, 3, 4], 50), 2)

    def test_first_draw_and_long_stall_not_filtered_out(self):
        stats = framestats(block(["1,1000000,1501000000,1502000000", "0,1600000000,1602000000,1603000000"]))
        self.assertEqual(stats["hwui_work"]["max_ms"], 1500)
        self.assertEqual(stats["hwui_work"]["samples"], 2)
        self.assertTrue(stats["complete"])

    def test_bad_sentinels(self):
        stats = framestats(block(["0,1,9223372036854775807,0", "0,0,0,0", "0,50,20,30"]))
        self.assertEqual(stats["invalid_rows"], 3)
        self.assertFalse(stats["complete"])

    def test_render_is_not_present(self):
        stats = framestats(block(["0,1,5000000", "0,5000001,8000000"], False))
        self.assertEqual(stats["hwui_work"]["samples"], 2)
        self.assertFalse(stats["complete"])
        self.assertIsNone(stats["presentation_source"])

    def test_duplicate_snapshots_and_time_filter(self):
        text = block(["0,1000000,2000000,3000000", "0,10000000,11000000,12000000"])
        stats = framestats(text + text, start_ns=5000000)
        self.assertEqual(stats["hwui_work"]["samples"], 1)
        self.assertEqual(stats["first_present_after_request_ms"], 7)

    def test_late_presentation_updates_existing_frame(self):
        text = block(["1,1000000,2000000,0", "0,11000000,12000000,13000000"])
        text += block(["1,1000000,2000000,3000000", "0,11000000,12000000,13000000"])
        result = framestats(text)
        self.assertTrue(result["complete"])
        self.assertEqual(result["hwui_work"]["samples"], 2)
        self.assertEqual(result["flagged_rows"], 1)

    def test_partial_present_data_is_incomplete(self):
        result = framestats(block(["0,1,1000000,2000000", "0,10000001,11000000,12000000", "0,20000001,21000000,0"]))
        self.assertFalse(result["complete"])
        self.assertEqual(result["presentation_missing_frames"], 1)

    def test_display_and_refresh(self):
        self.assertEqual(display_size("Physical size: 1920x1200\nOverride size: 1600x1000"), (1600, 1000))
        self.assertEqual(sf_period_ns("6060606\n0 0 0"), 6060606)
        with self.assertRaises(ValueError):
            sf_period_ns("0")

    def test_launch_failure_not_success(self):
        with self.assertRaises(ValueError):
            activity_launch("Error: Activity class does not exist")
        self.assertEqual(activity_launch("Status: ok\nTotalTime: 99")["TotalTime"], 99)


if __name__ == "__main__":
    unittest.main()
