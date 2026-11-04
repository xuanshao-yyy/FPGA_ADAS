"""Behavior checks: uncertainty, offset direction, persistence, command alarm."""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'code'))

import unittest

import cv2
import numpy as np

from lane_assist import Boundary, LaneAssist, extract_boundaries, marking_style


def pair(center=.5, styles=('UNKNOWN', 'UNKNOWN')):
    # .4 lane width at y=.9, .2 at y=.65, vanishing point at .4
    return [Boundary(np.array([-.4, center + .16]), .65, .94, styles[0]),
            Boundary(np.array([.4, center - .16]), .65, .94, styles[1])]


class LaneTests(unittest.TestCase):
    def test_offset_direction_and_missing_reset(self):
        s = LaneAssist(fps=10)
        for _ in range(5): result = s.update(pair(center=.38))
        self.assertEqual(result['state'], 'DEVIATION RIGHT')
        self.assertGreater(result['offset'], .45)
        self.assertTrue(result['alarm'])
        self.assertEqual(s.update([])['state'], 'UNKNOWN')
        self.assertEqual(s.update(pair())['state'], 'CHECKING')

    def test_forced_solid_and_dashed_candidate(self):
        s = LaneAssist(fps=10)
        for _ in range(5): result = s.update(pair(styles=('SOLID_CANDIDATE', 'DASHED_CANDIDATE')), 'left', True)
        self.assertTrue(result['alarm'])
        self.assertEqual(result['action'], 'ALARM: FORCED SOLID CHANGE')
        result = s.update(pair(styles=('SOLID_CANDIDATE', 'DASHED_CANDIDATE')), 'right')
        self.assertFalse(result['alarm'])
        self.assertEqual(result['action'], 'DASHED CANDIDATE: CHECK SIDE')
        # Missing side evidence must never authorize the simulated maneuver.
        self.assertEqual(s.update([], 'right', True)['action'], 'WAIT: LANE UNAVAILABLE')

    def test_flickering_style_not_confirmed(self):
        s = LaneAssist(fps=10)
        for i in range(20):
            style = 'SOLID_CANDIDATE' if i % 2 else 'UNKNOWN'
            result = s.update(pair(styles=(style, 'UNKNOWN')), 'left', True)
            self.assertFalse(result['alarm'])
            self.assertEqual(result['left'], 'UNKNOWN')

    def test_horizontal_crosswalk_not_lane(self):
        mask = np.zeros((540, 960), np.uint8)
        for y in range(350, 500, 25): cv2.line(mask, (80, y), (900, y), 255, 5)
        self.assertEqual(extract_boundaries(mask, mask, np.zeros_like(mask)), [])

    def test_occlusion_does_not_create_dashed_evidence(self):
        paint = np.zeros((540, 960), np.uint8)
        cv2.line(paint, (300, 300), (300, 510), 255, 6)
        boundary = Boundary(np.array([0., 300 / 960]), 300 / 540, 510 / 540)
        blocked = np.zeros_like(paint)
        blocked[330:380] = 255
        blocked[420:480] = 255
        self.assertEqual(marking_style(boundary, paint, blocked), 'SOLID_CANDIDATE')
        blocked[300:510] = 255
        self.assertEqual(marking_style(boundary, paint, blocked), 'UNKNOWN')


if __name__ == '__main__': unittest.main()
