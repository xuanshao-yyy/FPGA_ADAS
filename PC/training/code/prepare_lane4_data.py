"""Convert existing BDD annotations, reuse original images, reject overwriting outputs."""
import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import random
import time
import zipfile

import cv2
import numpy as np
from PIL import Image

from project_paths import PROJECT_ROOT
from lane4_data import make_mask, geometry, overlay_labels, NAMES, IGNORE


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=PROJECT_ROOT / 'datasets/lane4_bdd_v1')
    parser.add_argument('--limit', type=int, default=0, help='Per-split pilot limit; 0 converts all 80k annotations')
    args = parser.parse_args()
    if args.limit < 0:
        parser.error('limit must be non-negative')
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    (out / 'previews').mkdir()
    source = PROJECT_ROOT / 'data_archives/bdd100k_labels.zip'
    start = time.perf_counter()
    stats = dict(source=str(source), source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
                 classes=NAMES, ignore_index=IGNORE, mask_shape=[384, 640], line_width_pixels=3,
                 supervision='Semantic centerline bands, not physical paint segmentation',
                 official_renderer_reference='https://github.com/bdd100k/bdd100k/blob/e02ac3e815d513cea8d2e2113127f33f56826381/bdd_data/show_labels.py',
                 no_lane_annotation_policy='Ignore entire image and exclude from supervised lists',
                 image_duplication=False, splits={}, errors=[])
    with zipfile.ZipFile(source) as archive:
        all_names = archive.namelist()
        for split in ['train', 'val']:
            (out / f'masks/{split}').mkdir(parents=True)
            names = sorted(name for name in all_names if name.startswith(f'100k/{split}/') and name.endswith('.json'))
            if args.limit:
                random.Random(42).shuffle(names)
                names = sorted(names[:args.limit])
            counts = np.zeros(4, np.int64)
            seen, excluded, classes, conditions = 0, 0, Counter(), Counter()
            previews, preview_counts = [], Counter()
            preview_records = []
            with (out / f'{split}.jsonl').open('x', encoding='utf-8') as index:
                for name in names:
                    data = json.loads(archive.read(name))
                    stem = Path(name).stem
                    image_path = PROJECT_ROOT / f'datasets/bdd8/images/{split}/{stem}.jpg'
                    frames = data.get('frames', [])
                    if len(frames) != 1:
                        stats['errors'].append(dict(source=name, error='Expected one annotated frame'))
                        continue
                    with Image.open(image_path) as image:
                        width, height = image.size
                    try:
                        target, details = make_mask(frames[0].get('objects', []), width, height)
                    except ValueError as error:
                        stats['errors'].append(dict(source=name, error=str(error)))
                        continue
                    seen += 1
                    mask_path = out / f'masks/{split}/{stem}.png'
                    if not cv2.imwrite(str(mask_path), target, [cv2.IMWRITE_PNG_COMPRESSION, 3]):
                        raise RuntimeError(f'Cannot write mask: {mask_path}')
                    histogram = np.bincount(target.ravel(), minlength=256)
                    counts += histogram[:4]
                    if details['no_lane_annotations']:
                        excluded += 1
                        continue
                    attributes = data.get('attributes', {})
                    condition = str(attributes.get('timeofday', 'unknown'))
                    conditions[condition] += 1
                    classes.update(str(cls) for cls in details['classes'])
                    # Dataset paths remain relocatable inside the shared project.
                    image_relative = os.path.relpath(image_path, out).replace('\\', '/')
                    row = dict(stem=stem, image=image_relative, mask=f'masks/{split}/{stem}.png',
                               width=width, height=height, source=name, classes=details['classes'],
                               timeofday=condition)
                    index.write(json.dumps(row) + '\n')
                    key = 'night' if condition == 'night' else ('crosswalk' if 3 in details['classes'] else
                          ('dashed' if 2 in details['classes'] else 'solid_or_negative'))
                    if preview_counts[key] < 3:
                        original = cv2.imread(str(image_path))
                        scale, left, top, rw, rh = geometry(width, height)
                        canvas = np.full((384, 640, 3), 114, np.uint8)
                        canvas[top:top+rh, left:left+rw] = cv2.resize(original, (rw, rh))
                        display = overlay_labels(canvas, target)
                        cv2.putText(display, f'{split} {key} | {stem}', (8, 27), cv2.FONT_HERSHEY_SIMPLEX,
                                    .47, (255, 255, 255), 1, cv2.LINE_AA)
                        filename = f'{split}_{key}_{preview_counts[key]:02}.jpg'
                        cv2.imwrite(str(out / 'previews' / filename), display)
                        previews.append(display)
                        preview_records.append(dict(file=filename, image=row['image'], source=name, classes=row['classes']))
                        preview_counts[key] += 1
                    if seen % 5000 == 0:
                        print('CONVERT', split, seen, '/', len(names), flush=True)
            stats['splits'][split] = dict(annotation_files=len(names), masks=seen,
                supervised_images=seen-excluded, excluded_no_annotations=excluded,
                class_pixel_counts=counts.tolist(), images_by_class=dict(classes), timeofday=dict(conditions))
            if previews:
                while len(previews) % 2:
                    previews.append(np.zeros_like(previews[0]))
                montage = np.vstack([np.hstack(previews[i:i+2]) for i in range(0, len(previews), 2)])
                cv2.imwrite(str(out / f'previews/{split}_overview.jpg'), montage)
            (out / f'previews/{split}_sources.json').write_text(json.dumps(preview_records, indent=2), encoding='utf-8')
            print('SPLIT DONE', split, stats['splits'][split], flush=True)
    stats['wall_seconds'] = time.perf_counter() - start
    (out / 'conversion_report.json').write_text(json.dumps(stats, indent=2), encoding='utf-8')
    config = dict(task='lane4_centerline_segmentation', classes=NAMES, ignore_index=IGNORE,
                  input_shape=[3, 384, 640], model='TwinLiteNetPlus Small lane branch with new four-class final block',
                  train='train.jsonl', val='val.jsonl', full_conversion=args.limit == 0,
                  conversion_errors=len(stats['errors']), line_width_pixels=3,
                  seed=42, batch=8, workers=2, pretrained_lr=3e-5, head_lr=3e-4,
                  weight_decay=.0005, epochs=30, patience=8)
    (out / 'dataset.json').write_text(json.dumps(config, indent=2), encoding='utf-8')
    if stats['errors']:
        raise RuntimeError(f'{len(stats["errors"])} conversion errors: inspect conversion_report.json before training')
    print('COMPLETE', out, flush=True)


if __name__ == '__main__':
    main()
