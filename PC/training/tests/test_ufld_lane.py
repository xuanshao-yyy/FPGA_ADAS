"""Check UFLD decode coordinate mapping, uncertainty, temporal hold and jumps."""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'code'))

import unittest
import numpy as np
import torch

from ufld_lane import decode_lanes, LaneCurveFilter, lane_offset


def curve(x):
    return np.column_stack([np.full(20, x), np.linspace(.5, .94, 20)]).astype(np.float32)


class UFLDTests(unittest.TestCase):
    def test_decode_row_and_column_axes(self):
        pred = dict(loc_row=torch.zeros(1, 200, 72, 4), loc_col=torch.zeros(1, 100, 81, 4),
                    exist_row=torch.zeros(1, 2, 72, 4), exist_col=torch.zeros(1, 2, 81, 4))
        pred['exist_row'][:, 1, :, 1] = 1
        pred['loc_row'][:, 99, :, 1] = 20
        pred['exist_col'][:, 1, :, 0] = 1
        pred['loc_col'][:, 49, :, 0] = 20
        lanes = decode_lanes(pred)
        self.assertEqual(set(lanes), {0, 1})
        np.testing.assert_allclose(lanes[1][:, 0], .5, atol=1e-5)
        np.testing.assert_allclose(lanes[0][:, 1], .5, atol=1e-5)
        self.assertAlmostEqual(float(lanes[1][0, 1]), .42, places=5)

    def test_prediction_expires_and_cannot_make_offset(self):
        filtering = LaneCurveFilter(fps=10)
        lanes = filtering.update({1: curve(.25), 2: curve(.75)})
        self.assertAlmostEqual(lane_offset(lanes), 0)
        held = filtering.update({})
        self.assertTrue(held[1]['predicted'])
        self.assertIsNone(lane_offset(held))
        filtering.update({})
        self.assertEqual(filtering.update({}), {})

    def test_large_jump_resets_and_small_jitter_smooths(self):
        filtering = LaneCurveFilter(fps=10)
        filtering.update({1: curve(.25)})
        smooth = filtering.update({1: curve(.27)})[1]['points'][:, 0]
        # Endpoint rows outside prior observed support are intentionally unsmoothed.
        self.assertTrue(np.all(smooth[2:-2] < .27))
        reset = filtering.update({1: curve(.5)})[1]['points'][:, 0]
        np.testing.assert_allclose(reset, .5)

    def test_no_extrapolation_for_offset(self):
        points = curve(.25); points[:, 1] -= .3
        lanes = {1: dict(points=points, predicted=False), 2: dict(points=curve(.75), predicted=False)}
        self.assertIsNone(lane_offset(lanes))


if __name__ == '__main__': unittest.main()
