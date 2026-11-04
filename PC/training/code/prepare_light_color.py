"""DRAFT crop preparation; not executed or approved for training (2026-10-18)."""
import argparse
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path
import xml.etree.ElementTree as ET
import zipfile

import cv2
import numpy as np

from light_color import NAMES, color_id, crop_bounds
from project_paths import PROJECT_ROOT as ROOT

DATA = ROOT / 'datasets/light_color_v1'


def digest(path):
    value = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(8 * 1024 * 1024), b''):
            value.update(block)
    return value.hexdigest()


def read_rows(path):
    with path.open(encoding='utf-8') as stream:
        for line in stream:
            yield json.loads(line)


def collect_existing():
    records = []
    states = ROOT / 'datasets/bdd8/traffic_light_states.jsonl'
    bdd = defaultdict(list)
    for record in read_rows(states):
        split = Path(record['image']).parts[1]
        if split not in ('train', 'val'):
            continue
        for index, light in enumerate(record['lights']):
            label = color_id(light['color'], 'bdd')
            if label is None:
                continue
            b = light['box2d']
            row = {'source': 'bdd', 'split': split, 'image': str(ROOT / 'datasets/bdd8' / record['image']),
                   'box': [b['x1'], b['y1'], b['x2'], b['y2']], 'label': label,
                   'origin_id': f"bdd/{record['image']}/{index}", 'annotation': light['color']}
            bdd[(split, label)].append(row)
    for (split, _), rows in bdd.items():
        rows.sort(key=lambda r: hashlib.sha256(r['origin_id'].encode()).digest())
        records.extend(rows[:6000] if split == 'train' else rows)
    for record in read_rows(ROOT / 'datasets/bosch_light/traffic_light_states.jsonl'):
        split = Path(record['image']).parts[1]
        if split not in ('train', 'val'):
            continue
        for index, light in enumerate(record['boxes']):
            label = color_id(light['label'], 'bosch')
            if label is None or light.get('occluded'):
                continue
            records.append({'source': 'bosch', 'split': split,
                            'image': str(ROOT / 'datasets/bosch_light' / record['image']),
                            'box': [light['x_min'], light['y_min'], light['x_max'], light['y_max']],
                            'label': label, 'origin_id': f"bosch/{record['image']}/{index}",
                            'annotation': light['label']})
    for record in read_rows(ROOT / 'datasets/traffic_light_v1/s2tld_states.jsonl'):
        split = 'train' if Path(record['image']).parts[1] == 's2tld_train' else 'val'
        for index, light in enumerate(record['lights']):
            label = color_id(light['color'], 's2tld')
            if label is None or light.get('difficult', '0') == '1' or light.get('truncated', '0') == '1':
                continue
            records.append({'source': 's2tld720', 'split': split,
                            'image': str(ROOT / 'datasets/traffic_light_v1' / record['image']),
                            'box': light['box_xyxy'], 'label': label,
                            'origin_id': f"s2tld720/{record['image']}/{index}",
                            'source_group': record['source_group'], 'annotation': light['color']})
    return records


def collect_external(output):
    archive = ROOT / 'data_archives/s2tld_1080x1920.zip'
    provenance = json.loads(archive.with_suffix('.source.json').read_text(encoding='utf-8'))
    if digest(archive) != provenance['sha256']:
        raise ValueError('New author archive checksum mismatch')
    raw = output / 'external_originals'
    raw.mkdir()
    records, source_groups = [], Counter()
    with zipfile.ZipFile(archive) as bundle:
        members = set(bundle.namelist())
        for number, member in enumerate(sorted(n for n in members if n.lower().endswith('.xml'))):
            root = ET.fromstring(bundle.read(member))
            image_member = member.replace('/Annotations/', '/JPEGImages/').rsplit('.', 1)[0] + '.jpg'
            if image_member not in members:
                alternatives = [m for m in members if Path(m).name == root.findtext('filename')]
                if len(alternatives) != 1:
                    raise ValueError(f'Cannot uniquely pair {member}')
                image_member = alternatives[0]
            image = raw / f'{number:05d}.jpg'
            image.write_bytes(bundle.read(image_member))
            group = str(Path(root.findtext('path', 'unavailable')).parent)
            source_groups[group] += 1
            for index, obj in enumerate(root.findall('object')):
                label = color_id(obj.findtext('name'), 's2tld')
                if label is None or obj.findtext('difficult', '0') == '1' or obj.findtext('truncated', '0') == '1':
                    continue
                if label == 3:
                    # Review found lit red/countdown content in example off boxes.
                    # Quarantine this source's four off annotations, do not relabel them.
                    continue
                box = obj.find('bndbox')
                records.append({'source': 's2tld1080', 'split': 'external', 'image': str(image),
                                # New 1080 XML includes zero-origin coordinates. Preserve provided
                                # edges for padded crops; do not silently apply the 720 parser.
                                'box': [float(box.findtext('xmin')), float(box.findtext('ymin')),
                                        float(box.findtext('xmax')), float(box.findtext('ymax'))],
                                'label': label, 'origin_id': f's2tld1080/{member}/{index}',
                                'annotation': obj.findtext('name'), 'source_group': group})
    return records, {'archive': provenance, 'images': sum(source_groups.values()),
                     'source_groups': dict(source_groups),
                     'role': 'Entire new 1080 source held out; never used to select checkpoints or thresholds'}


def prepare(output):
    cv2.setNumThreads(1)
    if output.exists():
        raise FileExistsError('Dataset exists; preserve it and select a new --output')
    output.mkdir(parents=True)
    records = collect_existing()
    extra, external_info = collect_external(output)
    records.extend(extra)
    # Validation has priority, then external evaluation; remove conflicting training duplicates.
    by_image = defaultdict(list)
    for record in records:
        by_image[(record['split'], record['image'])].append(record)
    split_rank = {'val': 0, 'external': 1, 'train': 2}
    inputs = sorted(by_image.items(), key=lambda item: (split_rank[item[0][0]], item[0][1]))
    streams = {s: (output / f'{s}.jsonl').open('w', encoding='utf-8') for s in split_rank}
    image_splits, crop_splits, skipped, statistics = {}, {}, Counter(), defaultdict(Counter)
    source_files = [ROOT / 'datasets/bdd8/traffic_light_states.jsonl',
                    ROOT / 'datasets/bosch_light/traffic_light_states.jsonl',
                    ROOT / 'datasets/traffic_light_v1/s2tld_states.jsonl']
    source_hashes = {str(p): digest(p) for p in source_files}
    try:
        for number, ((split, image_path), lights) in enumerate(inputs, 1):
            image_bytes = Path(image_path).read_bytes()
            image_sha = hashlib.sha256(image_bytes).hexdigest()
            if image_sha in image_splits and image_splits[image_sha] != split:
                skipped['cross_split_identical_image'] += len(lights)
                continue
            image_splits[image_sha] = split
            image = cv2.imdecode(np.frombuffer(image_bytes, dtype=np.uint8), cv2.IMREAD_COLOR)
            if image is None:
                raise ValueError(f'Cannot decode {image_path}')
            height, width = image.shape[:2]
            for row in lights:
                x1, y1, x2, y2 = row['box']
                if min(x2 - x1, y2 - y1) < 6:
                    skipped['short_edge_under_6_pixels'] += 1
                    continue
                bounds = crop_bounds(row['box'], width, height)
                # Reject padding that includes a second annotated lamp of another known color.
                a, b, c, d = bounds
                if any(other is not row and other['label'] != row['label'] and
                       a < (other['box'][0] + other['box'][2]) / 2 < c and
                       b < (other['box'][1] + other['box'][3]) / 2 < d for other in lights):
                    skipped['different_color_lamp_inside_crop'] += 1
                    continue
                crop = image[b:d, a:c]
                crop_sha = hashlib.sha256(str(crop.shape).encode() + crop.tobytes()).hexdigest()
                if crop_sha in crop_splits:
                    old_split, old_label = crop_splits[crop_sha]
                    skipped['conflicting_identical_crop' if old_label != row['label'] else
                            'cross_split_identical_crop' if old_split != split else 'duplicate_crop'] += 1
                    continue
                crop_splits[crop_sha] = (split, row['label'])
                name = hashlib.sha256(row['origin_id'].encode()).hexdigest()[:24] + '.jpg'
                destination = output / 'crops' / split / row['source'] / name
                destination.parent.mkdir(parents=True, exist_ok=True)
                if not cv2.imwrite(str(destination), crop, [cv2.IMWRITE_JPEG_QUALITY, 95]):
                    raise RuntimeError(f'Cannot save crop: {destination}')
                row.update(crop=str(destination.relative_to(output)), crop_bounds=bounds,
                           crop_pixel_sha256=crop_sha, original_size=[width, height])
                streams[split].write(json.dumps(row, ensure_ascii=False) + '\n')
                statistics[f"{split}/{row['source']}"][NAMES[row['label']]] += 1
            if number % 1000 == 0:
                print(f'Prepared images {number}/{len(inputs)}', flush=True)
    finally:
        for stream in streams.values():
            stream.close()
    assert all(digest(Path(p)) == expected for p, expected in source_hashes.items())
    report = {'names': NAMES, 'input_size': 128, 'padding_fraction_each_side': .08,
              'statistics': dict(statistics), 'skipped': dict(skipped), 'source_hashes': source_hashes,
              'external': external_info, 'original_annotations_unchanged': True,
              'scope': 'Four colors only; ground-truth crops; no arrow, relevance or non-signal training',
              'splits': 'BDD original split; Bosch whole recordings; S2TLD720 original derived source groups; all 1080 held out',
              'limitations': 'Exact image and crop duplicates removed; near-duplicate scenes not proven absent. '
                             'Excludes very small/known occluded/difficult crops; these metrics do not establish end-to-end performance.'}
    (output / 'dataset.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps({'statistics': report['statistics'], 'skipped': report['skipped']}, indent=2), flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=DATA)
    args = parser.parse_args()
    prepare(args.output.resolve())
