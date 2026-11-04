"""Meaningful conversion contracts: official curves, semantic classes and uncertainty."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'code'))
import numpy as np
from matplotlib.path import Path as MplPath
from lane4_data import sample_polyline, make_mask, classify, IGNORE


def line(category, style='solid', direction='parallel', y=500):
    return dict(category=category, attributes=dict(style=style, direction=direction),
                poly2d=[[200, y, 'L'], [1000, y, 'L']])


class Lane4DataTests(unittest.TestCase):
    def test_official_curve_endpoint_and_not_control_polygon(self):
        poly = [[0, 0, 'C'], [0, 100, 'C'], [100, 100, 'C'], [100, 0, 'L']]
        points = sample_polyline(poly)
        np.testing.assert_array_equal(points[0], [0, 0])
        np.testing.assert_array_equal(points[-1], [100, 0])
        self.assertLessEqual(int(points[:, 1].max()), 75)
        self.assertGreaterEqual(int(points[:, 1].max()), 74)

    def test_mixed_curve_matches_official_consumed_commands(self):
        poly = [[0, 0, 'L'], [0, 80, 'C'], [80, 80, 'C'], [80, 0, 'C'], [100, 0, 'L']]
        codes = [MplPath.MOVETO] + [MplPath.CURVE4 if p[2] == 'C' else MplPath.LINETO for p in poly[1:]]
        segments = list(MplPath([p[:2] for p in poly], codes).iter_segments(simplify=False, curves=True))
        self.assertEqual([s[1] for s in segments], [MplPath.MOVETO, MplPath.CURVE4, MplPath.LINETO])
        points = sample_polyline(poly)
        self.assertTrue(np.any(np.all(points == [80, 0], axis=1)))
        np.testing.assert_array_equal(points[-1], [100, 0])

    def test_style_direction_and_crosswalk(self):
        self.assertEqual(classify(line('lane/single white', 'dashed')), 2)
        self.assertEqual(classify(line('lane/single yellow')), 1)
        self.assertEqual(classify(line('lane/crosswalk', 'dashed', 'vertical')), 3)
        self.assertEqual(classify(line('lane/road curb')), 0)
        self.assertEqual(classify(line('lane/single white', direction='vertical')), 0)
        self.assertEqual(classify(line('lane/single white', style='unknown')), IGNORE)

    def test_padding_and_missing_annotations_are_ignored(self):
        target, info = make_mask([], 1280, 720)
        self.assertTrue((target == IGNORE).all())
        self.assertTrue(info['no_lane_annotations'])
        target, _ = make_mask([line('lane/single white')], 1280, 720)
        self.assertTrue((target[:12] == IGNORE).all())
        self.assertTrue((target[-12:] == IGNORE).all())
        self.assertEqual(int(target[262, 300]), 1)

    def test_overlaps_do_not_overwrite_another_class(self):
        target, info = make_mask([line('lane/single white'), line('lane/single yellow', 'dashed')], 1280, 720)
        self.assertEqual(int(target[262, 300]), IGNORE)
        self.assertGreater(info['conflict_pixels'], 0)

    def test_bad_curve_is_rejected(self):
        with self.assertRaises(ValueError):
            sample_polyline([[0, 0, 'L'], [1, 1, 'C']])


if __name__ == '__main__':
    unittest.main()
