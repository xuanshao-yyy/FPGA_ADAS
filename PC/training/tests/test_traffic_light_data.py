"""Class mapping and validation checks for the independent traffic-light task."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'code'))
from train_traffic_light import light_rows
from prepare_s2tld import voc_rows


class TrafficLightDataTests(unittest.TestCase):
    def test_bdd_mapping_ignores_people_and_deduplicates_lights(self):
        labels = '0 0.5 0.5 0.1 0.1\n7 0.6 0.3 0.01 0.02\n7 0.6 0.3 0.01 0.02\n'
        self.assertEqual(light_rows(labels, 7), ['0 0.6 0.3 0.01 0.02'])

    def test_bosch_class_zero_is_a_light(self):
        self.assertEqual(light_rows('0 0.5 0.5 0.1 0.1\n', 0), ['0 0.5 0.5 0.1 0.1'])

    def test_negative_image_is_valid(self):
        self.assertEqual(light_rows('', 0), [])
        self.assertEqual(light_rows('2 0.5 0.5 0.1 0.1', 7), [])

    def test_invalid_labels_fail_before_training(self):
        for label in ('7 nan 0.5 0.1 0.1', '7 0.5 0.5 0 0.1', '7.5 0.5 0.5 0.1 0.1', '7 0.5 0.5'):
            with self.subTest(label=label), self.assertRaises(ValueError):
                light_rows(label, 7)

    def test_voc_origin_and_full_image_extent(self):
        xml = b'<annotation><size><width>1280</width><height>720</height></size><path>/capture/images/1.jpg</path><object><name>red</name><bndbox><xmin>1</xmin><ymin>1</ymin><xmax>1280</xmax><ymax>720</ymax></bndbox></object></annotation>'
        rows, states, group = voc_rows(xml)
        self.assertEqual(rows, ['7 0.50000000 0.50000000 1.00000000 1.00000000'])
        self.assertEqual(states[0]['box_xyxy'], [0, 0, 1280, 720])
        self.assertEqual(group, '/capture/images')


if __name__ == '__main__':
    unittest.main()
