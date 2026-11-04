"""Check ID continuity, confidence recovery, bounded coasting and smoothing."""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'code'))

import unittest
import numpy as np
from road_tracker import RoadTracker


def detection(x=100, score=.9, category=2):
    return np.array([[x, 100, x + 80, 180, score, category]], dtype=np.float32)


class TrackerTests(unittest.TestCase):
    def test_low_scores_keep_id_and_smoothing_reduces_jitter(self):
        tracker = RoadTracker()
        identities, raw, smooth = [], [], []
        for frame in range(30):
            x = 100 + frame * 2 + (3 if frame % 2 else -3)
            outputs = tracker.update(detection(x, .15 if frame in (10, 11) else .9))
            if outputs:
                self.assertEqual(len(outputs), 1)
                identities.append(outputs[0]['id'])
                raw.append(x)
                smooth.append(outputs[0]['box'][0])
                self.assertFalse(outputs[0]['predicted'])
        self.assertEqual(len(set(identities)), 1)
        self.assertEqual(len(identities), 29)
        self.assertLess(np.std(np.diff(smooth, n=2)), np.std(np.diff(raw, n=2)))

    def test_coasting_is_bounded_and_reactivation_keeps_id(self):
        tracker = RoadTracker()
        tracker.update(detection())
        identity = tracker.update(detection())[0]['id']
        for _ in range(2):
            output = tracker.update([])
            self.assertEqual(output[0]['id'], identity)
            self.assertTrue(output[0]['predicted'])
        self.assertEqual(tracker.update([]), [])
        output = tracker.update(detection())
        self.assertEqual(output[0]['id'], identity)
        self.assertFalse(output[0]['predicted'])

    def test_one_frame_detection_is_not_displayed(self):
        tracker = RoadTracker()
        self.assertEqual(tracker.update(detection()), [])
        self.assertEqual(tracker.update([]), [])

    def test_categories_do_not_share_ids(self):
        tracker = RoadTracker()
        tracker.update(detection(category=2))
        car = tracker.update(detection(category=2))[0]['id']
        tracker.update(detection(category=4))
        outputs = tracker.update(detection(category=4))
        truck = next(row for row in outputs if row['cls'] == 4)
        self.assertNotEqual(truck['id'], car)

    def test_empty_frames_and_inference_rate(self):
        tracker = RoadTracker(fps=10)
        self.assertEqual(tracker.hold_frames, 1)
        self.assertEqual(tracker.update([]), [])


if __name__ == '__main__':
    unittest.main()
