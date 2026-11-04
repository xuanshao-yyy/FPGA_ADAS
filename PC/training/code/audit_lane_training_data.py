"""Read-only audit of raw BDD lane labels and matching existing images.

Writes only an incremented report folder. This does not produce UFLD training
targets: Bezier conversion and lane-slot assignment need separate validation.
"""
from collections import Counter
import json
from pathlib import Path
from project_paths import PROJECT_ROOT, DATA_ARCHIVES
import zipfile

from ultralytics.utils.files import increment_path


def main():
    root = PROJECT_ROOT
    output = increment_path(root / 'datasets/reports/lane_audit', mkdir=True)
    stats = dict(source=str(DATA_ARCHIVES / 'bdd100k_labels.zip'), image_root=str(root / 'datasets/bdd8/images'),
                 ufld_training_targets_ready=False, splits={})
    mark_categories = {'lane/single white', 'lane/single yellow', 'lane/single other',
                       'lane/double white', 'lane/double yellow', 'lane/double other'}
    with zipfile.ZipFile(DATA_ARCHIVES / 'bdd100k_labels.zip') as archive:
        for split in ('train', 'val'):
            categories, styles, directions, point_types = Counter(), Counter(), Counter(), Counter()
            with_lanes, with_marks, files, missing, malformed, negative, points = 0, 0, 0, [], [], 0, 0
            examples = []
            for name in sorted(archive.namelist()):
                if not name.startswith(f'100k/{split}/') or not name.endswith('.json'): continue
                data = json.loads(archive.read(name)); files += 1
                stem = Path(name).stem
                image = root / 'datasets/bdd8/images' / split / f'{stem}.jpg'
                if not image.is_file(): missing.append(stem)
                lanes = [o for frame in data.get('frames', []) for o in frame.get('objects', [])
                         if o['category'].startswith('lane/')]
                marks = [o for o in lanes if o['category'] in mark_categories]
                with_lanes += bool(lanes); with_marks += bool(marks)
                negative += not bool(marks)
                for lane in lanes:
                    categories[lane['category']] += 1
                    if lane['category'] in mark_categories:
                        styles[lane.get('attributes', {}).get('style', 'MISSING')] += 1
                        directions[lane.get('attributes', {}).get('direction', 'MISSING')] += 1
                    poly = lane.get('poly2d')
                    if not isinstance(poly, list) or len(poly) < 2:
                        malformed.append(dict(file=name, id=lane.get('id'))); continue
                    for point in poly:
                        if not isinstance(point, list) or len(point) != 3:
                            malformed.append(dict(file=name, id=lane.get('id'))); continue
                        point_types[str(point[2])] += 1; points += 1
                if len(examples) < 12 and marks:
                    examples.append(dict(image=str(image), source=name, markings=marks))
                if files % 10000 == 0: print(f'AUDIT {split} {files}', flush=True)
            stats['splits'][split] = dict(annotation_files=files, images_with_any_lane_label=with_lanes,
                images_with_markings=with_marks, images_without_markings=negative,
                missing_images=missing, categories=dict(categories), marking_styles=dict(styles),
                marking_directions=dict(directions), point_types=dict(point_types),
                malformed_polylines=malformed)
            (output / f'{split}_examples.json').write_text(json.dumps(examples, indent=2), encoding='utf-8')
            print(json.dumps({split: stats['splits'][split]}, indent=2), flush=True)
    (output / 'report.json').write_text(json.dumps(stats, indent=2), encoding='utf-8')
    print(f'COMPLETE {output}', flush=True)


if __name__ == '__main__': main()
