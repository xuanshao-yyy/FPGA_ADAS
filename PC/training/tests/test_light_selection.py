"""Check conservative color eligibility; no model is loaded or trained."""
import importlib.util
from pathlib import Path
import unittest

path = Path(__file__).resolve().parents[1] / 'maintenance/light_selection_20261018/select_data.py'
spec = importlib.util.spec_from_file_location('light_selection_review', path)
selection = importlib.util.module_from_spec(spec)
spec.loader.exec_module(selection)


def lamp(color=0, box=(10, 10, 30, 30), **flags):
    return dict(color_id=color, box=box, occluded=False, difficult=False, truncated=False) | flags


class ColorEligibility(unittest.TestCase):
    def test_explicit_colors_can_be_candidates(self):
        for value in (0, 1, 2):
            light = lamp(value)
            self.assertIsNone(selection.color_reason(light, (100, 100), [light]))

    def test_none_and_off_are_not_rgb_training_truth(self):
        for value in (None, 3):
            light = lamp(value)
            self.assertEqual(selection.color_reason(light, (100, 100), [light]), 'no_explicit_red_yellow_green')

    def test_visibility_flags_reject_color_only(self):
        for flag in ('occluded', 'difficult', 'truncated'):
            light = lamp(**{flag: True})
            self.assertEqual(selection.color_reason(light, (100, 100), [light]), 'visibility_flag')

    def test_tiny_and_out_of_frame_crops_need_review(self):
        for box, reason in [((10, 10, 17, 30), 'native_short_edge_under_8'),
                            ((-1, 10, 30, 30), 'color_box_crosses_image_edge'),
                            ((10, 10, 10, 30), 'invalid_box')]:
            light = lamp(box=box)
            self.assertEqual(selection.color_reason(light, (100, 100), [light]), reason)

    def test_unknown_or_other_color_inside_crop_rejected(self):
        light = lamp()
        for value in (None, 1, 2, 3):
            other = lamp(value, (12, 12, 22, 22))
            self.assertEqual(selection.color_reason(light, (100, 100), [light, other]),
                             'different_or_unknown_lamp_center_inside_crop')


if __name__ == '__main__':
    unittest.main()
