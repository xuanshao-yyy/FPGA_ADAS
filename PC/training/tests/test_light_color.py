"""Semantic checks for draft color preparation. No training or optimizer steps."""
from pathlib import Path
import sys
import unittest

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'code'))
from light_color import color_id, crop_bounds, letterbox_rgb, tensor_rgb, classification_metrics, hsv_color


class LightColorContracts(unittest.TestCase):
    def test_unlabelled_is_not_extinguished(self):
        self.assertIsNone(color_id('none', 'bdd'))
        self.assertIsNone(color_id('wait_on', 's2tld'))
        self.assertEqual(color_id('off', 's2tld'), 3)

    def test_arrow_label_can_supply_color_without_inventing_shape(self):
        self.assertEqual(color_id('RedStraightLeft', 'bosch'), 0)
        self.assertEqual(color_id('GreenRight', 'bosch'), 2)
        self.assertIsNone(color_id('pedestrian', 'bosch'))

    def test_crop_preserves_border_target_and_clamps_padding(self):
        self.assertEqual(crop_bounds([0, 0, 10, 20], 20, 30), (0, 0, 11, 22))
        with self.assertRaises(ValueError):
            crop_bounds([20, 2, 10, 6], 40, 40)
        with self.assertRaises(ValueError):
            crop_bounds([float('nan'), 0, 10, 10], 40, 40)

    def test_preprocessing_keeps_rgb_and_full_lamp_aspect(self):
        image = np.zeros((10, 40, 3), dtype=np.uint8)
        image[:] = [255, 0, 0]
        padded = letterbox_rgb(image)
        self.assertTrue(np.all(padded[0] == 114))
        self.assertEqual(padded[64, 64].tolist(), [255, 0, 0])
        tensor = tensor_rgb(image)
        self.assertEqual(tuple(tensor.shape), (3, 128, 128))
        self.assertGreater(float(tensor[0, 64, 64]), float(tensor[2, 64, 64]))

    def test_abstentions_remain_missed_targets(self):
        result = classification_metrics([0, 0, 2], [0, 4, 2])
        self.assertAlmostEqual(result['accuracy'], 2 / 3)
        self.assertEqual(result['per_class']['red']['recall'], .5)
        self.assertAlmostEqual(result['coverage'], 2 / 3)
        self.assertEqual(classification_metrics([0], [2])['red_predicted_green'], 1)

    def test_hsv_rejects_blank_instead_of_claiming_off(self):
        self.assertEqual(hsv_color(np.zeros((20, 20, 3), dtype=np.uint8)), 4)
        for rgb, expected in [([255, 0, 0], 0), ([255, 255, 0], 1), ([0, 255, 0], 2)]:
            image = np.zeros((20, 20, 3), dtype=np.uint8)
            image[6:12, 6:12] = rgb
            self.assertEqual(hsv_color(image), expected)


if __name__ == '__main__':
    unittest.main()
