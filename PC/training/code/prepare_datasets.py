"""Prepare the verified local archives without changing their contents.

Run with the Anaconda base Python (Pillow and PyYAML are required).
The output directory must not already exist, preventing accidental overwrite.
"""
import collections
import concurrent.futures
import csv
import hashlib
import io
import json
import math
from pathlib import Path
import shutil
import zipfile

from PIL import Image, ImageDraw, ImageFont
import yaml

from project_paths import PROJECT_ROOT as ROOT, DATA_ARCHIVES
OUT = ROOT / 'datasets'
NAMES = ['person', 'rider', 'car', 'bus', 'truck', 'bicycle', 'motorcycle', 'traffic_light']
MAPPING = {'person': 0, 'rider': 1, 'car': 2, 'bus': 3, 'truck': 4,
           'bike': 5, 'motor': 6, 'traffic light': 7}
SPLITS = ('train', 'val', 'test')


def log(message):
    print(message, flush=True)


def write_json(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, ensure_ascii=False, indent=2), encoding='utf-8')


def write_yaml(folder, filename, names, **splits):
    data = {'path': folder.as_posix(), **splits, 'names': dict(enumerate(names))}
    (folder / filename).write_text(yaml.safe_dump(data, sort_keys=False), encoding='utf-8')


def make_dirs(folder):
    for split in SPLITS:
        (folder / 'images' / split).mkdir(parents=True)
        (folder / 'labels' / split).mkdir(parents=True)


def copy_member(archive, member, destination):
    destination.parent.mkdir(parents=True, exist_ok=True)
    with archive.open(member) as source, destination.open('wb') as target:
        shutil.copyfileobj(source, target)


def box_line(class_id, coordinates, width=1280, height=720):
    x1, y1, x2, y2 = map(float, coordinates)
    if not all(math.isfinite(v) for v in (x1, y1, x2, y2)):
        return None
    x1, x2 = max(0, min(width, x1)), max(0, min(width, x2))
    y1, y2 = max(0, min(height, y1)), max(0, min(height, y2))
    if x2 <= x1 or y2 <= y1:
        return None
    return f'{class_id} {(x1+x2)/(2*width):.8f} {(y1+y2)/(2*height):.8f} {(x2-x1)/width:.8f} {(y2-y1)/height:.8f}'


def prepare_bdd():
    folder = OUT / 'bdd8'
    make_dirs(folder)
    with zipfile.ZipFile(DATA_ARCHIVES / 'bdd100k_images_100k.zip') as archive:
        members = [n for n in archive.namelist() if n.endswith('.jpg')]
        for index, member in enumerate(members, 1):
            parts = member.split('/')
            if len(parts) != 3 or parts[0] != '100k' or parts[1] not in SPLITS:
                raise ValueError(f'Unexpected BDD image path: {member}')
            copy_member(archive, member, folder / 'images' / parts[1] / parts[2])
            if index % 10000 == 0:
                log(f'BDD images extracted: {index}/{len(members)}')
    stats = {}
    records = {s: [] for s in SPLITS}
    invalid_path = OUT / 'reports' / 'bdd_invalid_boxes.csv'
    with zipfile.ZipFile(DATA_ARCHIVES / 'bdd100k_labels.zip') as archive, \
            invalid_path.open('w', newline='', encoding='utf-8-sig') as error_file, \
            (folder / 'traffic_light_states.jsonl').open('w', encoding='utf-8') as states_file:
        writer = csv.writer(error_file)
        writer.writerow(['source_json', 'category', 'x1', 'y1', 'x2', 'y2', 'selected_class'])
        for split in SPLITS:
            counter = collections.Counter()
            invalid_selected = invalid_all = empty = 0
            members = [n for n in archive.namelist() if n.startswith(f'100k/{split}/') and n.endswith('.json')]
            for index, member in enumerate(members, 1):
                data = json.loads(archive.read(member))
                stem = Path(member).stem
                image_path = folder / 'images' / split / f'{stem}.jpg'
                if not image_path.is_file() or data['name'] != stem:
                    raise ValueError(f'BDD image/annotation mismatch: {member}')
                lines, classes, states = [], set(), []
                for frame in data.get('frames', []):
                    for obj in frame.get('objects', []):
                        if 'box2d' not in obj:
                            continue
                        category = obj['category']
                        box = obj['box2d']
                        coords = [box[k] for k in ('x1', 'y1', 'x2', 'y2')]
                        class_id = MAPPING.get(category)
                        line = box_line(class_id if class_id is not None else 0, coords)
                        if line is None:
                            invalid_all += 1
                            invalid_selected += class_id is not None
                            writer.writerow([member, category, *coords, class_id is not None])
                            continue
                        if class_id is None:
                            continue
                        lines.append(line)
                        counter[NAMES[class_id]] += 1
                        classes.add(class_id)
                        if category == 'traffic light':
                            states.append({'box2d': box, 'color': obj.get('attributes', {}).get('trafficLightColor', 'none')})
                (folder / 'labels' / split / f'{stem}.txt').write_text('\n'.join(lines) + ('\n' if lines else ''), encoding='ascii')
                if states:
                    states_file.write(json.dumps({'image': f'images/{split}/{stem}.jpg', 'lights': states}) + '\n')
                empty += not lines
                records[split].append((image_path, classes))
                if index % 10000 == 0:
                    log(f'BDD labels converted ({split}): {index}/{len(members)}')
            stats[split] = {'images': len(members), 'objects': dict(counter),
                            'empty_labels': empty, 'invalid_source_boxes': invalid_all,
                            'invalid_selected_boxes_skipped': invalid_selected}
    write_yaml(folder, 'bdd8.yaml', NAMES, train='images/train', val='images/val', test='images/test')
    subset_stats = {}
    for prefix, sizes, minimum in [('smoke', (300, 100), 10), ('starter', (5000, 1000), 60)]:
        subset_stats[prefix] = {}
        for split, size in zip(('train', 'val'), sizes):
            ordered = sorted(records[split], key=lambda r: hashlib.sha256(r[0].name.encode()).digest())
            selected, seen, coverage = [], set(), collections.Counter()
            for class_id in (6, 1, 5, 3, 4, 0, 7, 2):
                for path, classes in ordered:
                    if coverage[class_id] >= minimum:
                        break
                    if class_id in classes and path not in seen:
                        selected.append((path, classes))
                        seen.add(path)
                        coverage.update(classes)
            for path, classes in ordered:
                if len(selected) >= size:
                    break
                if path not in seen:
                    selected.append((path, classes))
                    seen.add(path)
                    coverage.update(classes)
            if len(selected) != size:
                raise ValueError(f'Unable to select {prefix}/{split} size {size}')
            (folder / f'{prefix}_{split}.txt').write_text(''.join(p.as_posix() + '\n' for p, _ in selected), encoding='utf-8')
            subset_stats[prefix][split] = {'images': size, 'images_containing_class': {NAMES[c]: coverage[c] for c in range(8)}}
        write_yaml(folder, f'bdd8_{prefix}.yaml', NAMES, train=f'{prefix}_train.txt', val=f'{prefix}_val.txt', test='images/test')
    stats['subsets'] = subset_stats
    return stats


def prepare_bosch():
    folder = OUT / 'bosch_light'
    make_dirs(folder)
    stats = {s: {'images': 0, 'objects': 0, 'empty_labels': 0, 'invalid_boxes_skipped': 0, 'sequences': []} for s in SPLITS}
    raw = OUT / 'source_annotations' / 'bosch'
    raw.mkdir(parents=True)
    validation_sequence = '2015-10-05-10-55-33_bag'
    test_sequence = '2017-02-03-11-44-56_los_altos_mountain_view_traffic_lights_bag'
    with zipfile.ZipFile(DATA_ARCHIVES / 'dataset_train_rgb.zip') as archive, \
            (folder / 'traffic_light_states.jsonl').open('w', encoding='utf-8') as states_file:
        annotations = yaml.safe_load(archive.read('dataset_train_rgb/train.yaml'))
        for member in ('dataset_train_rgb/train.yaml', 'dataset_train_rgb/non-commercial_license.docx'):
            copy_member(archive, member, raw / Path(member).name)
        for index, record in enumerate(annotations, 1):
            path = record['path'].removeprefix('./')
            sequence = path.split('/')[-2]
            split = 'val' if sequence == validation_sequence else 'test' if sequence == test_sequence else 'train'
            basename = sequence + '__' + Path(path).name
            destination = folder / 'images' / split / basename
            if destination.exists():
                raise ValueError(f'Duplicate Bosch name: {basename}')
            copy_member(archive, 'dataset_train_rgb/' + path, destination)
            lines = []
            for box in record.get('boxes', []):
                line = box_line(0, [box[k] for k in ('x_min', 'y_min', 'x_max', 'y_max')])
                if line is None:
                    stats[split]['invalid_boxes_skipped'] += 1
                else:
                    lines.append(line)
            (folder / 'labels' / split / (Path(basename).stem + '.txt')).write_text('\n'.join(lines) + ('\n' if lines else ''), encoding='ascii')
            states_file.write(json.dumps({'image': f'images/{split}/{basename}', 'source_path': path, 'boxes': record.get('boxes', [])}) + '\n')
            stats[split]['images'] += 1
            stats[split]['objects'] += len(lines)
            stats[split]['empty_labels'] += not lines
            if sequence not in stats[split]['sequences']:
                stats[split]['sequences'].append(sequence)
            if index % 1000 == 0:
                log(f'Bosch converted: {index}/{len(annotations)}')
    with zipfile.ZipFile(DATA_ARCHIVES / 'bstld-master.zip') as archive:
        for member in archive.namelist():
            if not member.endswith('/'):
                copy_member(archive, member, OUT / 'source_annotations' / member)
    write_yaml(folder, 'bosch_light.yaml', ['traffic_light'], train='images/train', val='images/val', test='images/test')
    return stats


def prepare_cone():
    folder = OUT / 'tracon_cone'
    make_dirs(folder)
    stats = {s: {'images': 0, 'objects': 0} for s in SPLITS}
    with zipfile.ZipFile(DATA_ARCHIVES / 'Cone-Detection-main.zip') as archive:
        prefix = 'Cone-Detection-main/TraCon_dataset/'
        for member in archive.namelist():
            if not member.startswith(prefix + 'images/') or Path(member).suffix.lower() not in ('.jpg', '.png'):
                continue
            relative = member.removeprefix(prefix)
            split = relative.split('/')[1]
            image_path = folder / relative
            copy_member(archive, member, image_path)
            label_member = prefix + f'labels/{split}/{Path(member).stem}.txt'
            lines = []
            for source_line in archive.read(label_member).decode().splitlines():
                if not source_line.strip():
                    continue
                fields = source_line.split()
                values = list(map(float, fields[1:]))
                if len(fields) != 5 or fields[0] != '0' or not all(math.isfinite(v) and 0 <= v <= 1 for v in values) or min(values[2:]) <= 0:
                    raise ValueError(f'Invalid cone annotation: {label_member}')
                lines.append(source_line)
            label_path = folder / 'labels' / split / (image_path.stem + '.txt')
            if label_path.exists():
                raise ValueError(f'Duplicate cone stem: {image_path.stem}')
            label_path.write_text('\n'.join(lines) + ('\n' if lines else ''), encoding='ascii')
            stats[split]['images'] += 1
            stats[split]['objects'] += len(lines)
        copy_member(archive, 'Cone-Detection-main/README.md', OUT / 'source_annotations' / 'TraCon_README.md')
    write_yaml(folder, 'tracon_cone.yaml', ['traffic_cone'], train='images/train', val='images/val', test='images/test')
    log('TraCon converted: 540 images')
    return stats


def verify_image(path):
    try:
        with Image.open(path) as image:
            image.load()
            size = image.size
        folder = path.parents[2]
        if folder.name in ('bdd8', 'bosch_light') and size != (1280, 720):
            raise ValueError(f'Unexpected dimensions: {size}')
        label = folder / 'labels' / path.parent.name / (path.stem + '.txt')
        if not label.is_file():
            raise ValueError('Missing label')
        max_id = 7 if folder.name == 'bdd8' else 0
        count = 0
        for line in label.read_text(encoding='ascii').splitlines():
            fields = line.split()
            if len(fields) != 5 or not 0 <= int(fields[0]) <= max_id:
                raise ValueError('Invalid YOLO row/class')
            x, y, w, h = map(float, fields[1:])
            if not all(math.isfinite(v) and 0 <= v <= 1 for v in (x, y, w, h)) or min(w, h) <= 0:
                raise ValueError('Invalid normalized coordinates')
            count += 1
        return None, count
    except Exception as error:
        return {'path': str(path), 'error': str(error)}, 0


def verify_all():
    images = sorted(p for p in OUT.glob('*/images/*/*') if p.suffix.lower() in ('.jpg', '.png'))
    errors, count = [], 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
        for index, (error, objects) in enumerate(pool.map(verify_image, images), 1):
            if error:
                errors.append(error)
            count += objects
            if index % 10000 == 0:
                log(f'Images decoded and labels verified: {index}/{len(images)}')
    orphan_labels = []
    for label in OUT.glob('*/labels/*/*.txt'):
        image_folder = label.parents[2] / 'images' / label.parent.name
        if not any((image_folder / (label.stem + suffix)).is_file() for suffix in ('.jpg', '.png')):
            orphan_labels.append(str(label))
    report = {'images_decoded': len(images), 'valid_label_rows': count, 'errors': errors, 'orphan_labels': orphan_labels}
    write_json(OUT / 'reports' / 'verification.json', report)
    if errors or orphan_labels:
        raise RuntimeError('Verification failed; see reports/verification.json')
    return report


def contact_sheet(dataset, split, names):
    folder = OUT / dataset
    images = sorted(folder.glob(f'images/{split}/*'), key=lambda p: hashlib.sha256(p.name.encode()).digest())
    selected, coverage = [], set()
    for path in images:
        label = folder / 'labels' / split / (path.stem + '.txt')
        classes = {int(line.split()[0]) for line in label.read_text().splitlines()}
        if classes - coverage:
            selected.append(path)
            coverage.update(classes)
        if len(coverage) == len(names):
            break
    for path in images:
        if len(selected) >= 12:
            break
        if path not in selected:
            selected.append(path)
    canvas = Image.new('RGB', (1920, 1560), '#151b25')
    font = ImageFont.truetype('C:/Windows/Fonts/arial.ttf', 13)
    colors = ['#ff5252', '#ffca28', '#00e5ff', '#76ff03', '#ff80ab', '#b388ff', '#ff9100', '#69f0ae']
    for index, path in enumerate(selected[:12]):
        with Image.open(path) as source:
            width, height = source.size
            tile = source.convert('RGB')
            tile.thumbnail((640, 360))
        draw = ImageDraw.Draw(tile)
        sx, sy = tile.width / width, tile.height / height
        for line in (folder / 'labels' / split / (path.stem + '.txt')).read_text().splitlines():
            cls, x, y, w, h = line.split()
            cls = int(cls)
            x, y, w, h = map(float, (x, y, w, h))
            x1, y1 = (x-w/2)*width*sx, (y-h/2)*height*sy
            x2, y2 = (x+w/2)*width*sx, (y+h/2)*height*sy
            draw.rectangle((x1, y1, x2, y2), outline=colors[cls], width=2)
            draw.text((x1, max(0, y1-14)), names[cls], fill=colors[cls], font=font, stroke_width=1, stroke_fill='black')
        left, top = (index % 3)*640, (index // 3)*390
        canvas.paste(tile, (left, top))
        ImageDraw.Draw(canvas).text((left+8, top+362), path.name[:76], fill='white', font=font)
    destination = OUT / 'reports' / f'{dataset}_{split}_samples.jpg'
    canvas.save(destination, quality=90)


def main():
    if OUT.exists():
        raise SystemExit(f'Refusing to overwrite existing output: {OUT}')
    archives = ['bdd100k_images_100k.zip', 'bdd100k_labels.zip', 'dataset_train_rgb.zip', 'Cone-Detection-main.zip', 'bstld-master.zip']
    for name in archives:
        with zipfile.ZipFile(DATA_ARCHIVES / name) as archive:
            for member in archive.namelist():
                path = Path(member)
                if path.is_absolute() or '..' in path.parts or ':' in member or '\\' in member:
                    raise ValueError(f'Unsafe archive member: {member}')
    (OUT / 'reports').mkdir(parents=True)
    write_json(OUT / 'reports' / 'source_archives.json', {'used': [{'file': name, 'bytes': (DATA_ARCHIVES/name).stat().st_size} for name in archives],
        'excluded': {'bdd100k_det_20_labels.zip': 'Archive contains 2000 JPEG test images and no JSON detection labels.'}})
    stats = {'bdd8': prepare_bdd(), 'bosch_light': prepare_bosch(), 'tracon_cone': prepare_cone()}
    write_json(OUT / 'reports' / 'dataset_stats.json', stats)
    verification = verify_all()
    for dataset, names in [('bdd8', NAMES), ('bosch_light', ['traffic_light']), ('tracon_cone', ['traffic_cone'])]:
        for split in ('train', 'val'):
            contact_sheet(dataset, split, names)
    log(f'COMPLETE: {verification["images_decoded"]} images decoded, no verification errors. Output: {OUT}')


if __name__ == '__main__':
    main()
