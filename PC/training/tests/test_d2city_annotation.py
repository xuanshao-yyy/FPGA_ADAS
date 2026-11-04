"""Check D2-City geometry conversion and exclusion of unfinished light labels."""
import csv
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'code'))
from prepare_d2city_annotation import (finalize, parse_annotations, split_for_video,
                                       validate_light_label, yolo_box)

TEMP_ROOT = Path(__file__).resolve().parents[1] / 'maintenance' / 'd2city_labeling_20261021' / 'test_fixtures'
TEMP_ROOT.mkdir(exist_ok=True)


class D2CityTests(unittest.TestCase):
    def test_box_clips_then_normalizes(self):
        box = ET.Element('box', xtl='-10', ytl='20', xbr='50', ybr='120')
        self.assertEqual(yolo_box(box, 100, 100), (.25, .6, .5, .8))

    def test_empty_and_nonfinite_boxes_are_rejected(self):
        for left, right in [('nan', '50'), ('120', '130'), ('40', '30')]:
            with self.subTest(left=left), self.assertRaises(ValueError):
                yolo_box(ET.Element('box', xtl=left, ytl='0', xbr=right, ybr='10'), 100, 100)

    def test_unknown_source_class_is_logged_without_remapping_to_light(self):
        with tempfile.TemporaryDirectory(dir=TEMP_ROOT) as folder:
            path = Path(folder) / 'test.xml'
            path.write_text('<annotations><meta><width>100</width><height>100</height></meta>'
                            '<track label="car"><box frame="7" xtl="0" ytl="0" xbr="10" ybr="10"/></track>'
                            '<track label="straight-forward"><box frame="7" xtl="0" ytl="0" xbr="10" ybr="10"/></track>'
                            '<track label="person"><box frame="9" outside="1"/></track></annotations>')
            width, height, frames, skipped = parse_annotations(path)
            self.assertEqual((width, height), (100, 100))
            self.assertEqual(len(frames[7]), 1)
            self.assertEqual(frames[7][0].split()[0], '0')
            self.assertEqual(frames[9], [])
            self.assertEqual(skipped[0]['label'], 'straight-forward')

    def test_unfinished_images_cannot_be_exported_as_negatives(self):
        with tempfile.TemporaryDirectory(dir=TEMP_ROOT) as folder:
            project = Path(folder)
            (project / 'annotation_queue.csv').write_text('image,video,split,status\na.jpg,a,train,pending\n')
            with self.assertRaisesRegex(ValueError, 'pending is not a negative'):
                finalize(SimpleNamespace(project=project))

    def test_invalid_light_id_and_out_of_image_box_fail(self):
        with tempfile.TemporaryDirectory(dir=TEMP_ROOT) as folder:
            path = Path(folder) / 'label.txt'
            for line in ['12 .5 .5 .2 .2', '0 .99 .5 .2 .2', '0 nan .5 .2 .2', '0 .5 .5 0 .2']:
                path.write_text(line)
                with self.subTest(line=line), self.assertRaises(ValueError):
                    validate_light_label(path)

    def test_reviewed_split_is_grouped_by_video(self):
        with tempfile.TemporaryDirectory(dir=TEMP_ROOT) as folder:
            project = Path(folder)
            for name in ['images', 'labels', 'reviewed']:
                (project / name).mkdir()
            videos = {}
            for i in range(100):
                video = f'video{i}'
                videos.setdefault(split_for_video(video), video)
            rows = []
            for split, video in videos.items():
                for frame, status in [(0, 'positive'), (1, 'negative')]:
                    image = f'{video}_{frame}.jpg'
                    (project / 'images' / image).touch()
                    (project / 'labels' / (Path(image).stem + '.txt')).write_text('0 .5 .5 .2 .2' if frame == 0 else '')
                    rows.append({'image': image, 'video': video, 'split': split, 'status': status})
            with (project / 'annotation_queue.csv').open('w', newline='') as stream:
                writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
                writer.writeheader()
                writer.writerows(rows)
            finalize(SimpleNamespace(project=project))
            for split, video in videos.items():
                lines = (project / 'reviewed' / (split + '.txt')).read_text().splitlines()
                self.assertEqual(len(lines), 2)
                self.assertTrue(all(video in name for name in lines))


if __name__ == '__main__':
    unittest.main()
