"""Prepare and fine-tune BDD8 with traffic-light-only partial annotation sources."""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path

from project_paths import PROJECT_ROOT

DATA = PROJECT_ROOT / 'datasets/traffic_light_v1'
START = PROJECT_ROOT / 'runs/bdd8_full_n640/weights/best.pt'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def light_rows(text, class_id):
    """Validate source detection labels, retain lights and map them to class zero."""
    rows = []
    seen = set()
    for line in text.splitlines():
        fields = line.split()
        if len(fields) != 5:
            raise ValueError(f'Expected detection label with five fields: {line}')
        cls, x, y, w, h = map(float, fields)
        if not all(math.isfinite(v) for v in (cls, x, y, w, h)):
            raise ValueError(f'Non-finite label: {line}')
        if cls != int(cls) or cls < 0 or not (0 <= x <= 1 and 0 <= y <= 1 and 0 < w <= 1 and 0 < h <= 1):
            raise ValueError(f'Invalid source label: {line}')
        if int(cls) != class_id:
            continue
        row = '0 ' + ' '.join(fields[1:])
        key = (x, y, w, h)
        if key not in seen:
            rows.append(row)
            seen.add(key)
    return rows


def order(path):
    return hashlib.sha256(path.name.encode()).hexdigest()


def select_bdd(items):
    """Size-stratified fine-tuning subset; keep a fixed selection of negatives."""
    groups = [[], [], []]
    negatives = []
    for item in items:
        if not item['rows']:
            negatives.append(item)
            continue
        # Original BDD is 1280x720; letterboxing to 640 scales each axis by 0.5.
        edge = min(min(float(r.split()[3]) * 640, float(r.split()[4]) * 360) for r in item['rows'])
        groups[0 if edge < 8 else 1 if edge < 16 else 2].append(item)
    selected = []
    remainder = []
    for group, quota in zip(groups, (6000, 4000, 2000)):
        group.sort(key=lambda item: order(item['image']))
        selected.extend(group[:quota])
        remainder.extend(group[quota:])
    selected.extend(sorted(remainder, key=lambda item: order(item['image']))[:max(0, 12000-len(selected))])
    selected.extend(sorted(negatives, key=lambda item: order(item['image']))[:3000])
    return selected


def bdd_train_selection():
    """Use the existing sequential annotation index to avoid reading all 70k TXT files."""
    root = PROJECT_ROOT / 'datasets/bdd8'
    images = {p.name: p for p in (root / 'images/train').glob('*.jpg')}
    positives = {}
    with (root / 'traffic_light_states.jsonl').open(encoding='utf-8') as stream:
        for line in stream:
            record = json.loads(line)
            if not record['image'].startswith('images/train/'):
                continue
            image = images[Path(record['image']).name]
            rows = []
            for light in record['lights']:
                box = light['box2d']
                x1, x2 = max(0., box['x1']), min(1280., box['x2'])
                y1, y2 = max(0., box['y1']), min(720., box['y2'])
                if x2 <= x1 or y2 <= y1:
                    raise ValueError(f'Invalid indexed box: {image}')
                rows.append(f'0 {(x1+x2)/2560} {(y1+y2)/1440} {(x2-x1)/1280} {(y2-y1)/720}')
            positives[image.name] = dict(image=image, rows=rows)
    indexed = list(positives.values()) + [dict(image=p, rows=[]) for name, p in images.items() if name not in positives]
    return [item['image'] for item in select_bdd(indexed)]


def inventory(dataset, split, class_id, selected_images=None):
    root = PROJECT_ROOT / 'datasets' / dataset
    result = []
    images = selected_images if selected_images is not None else sorted(
        p for p in (root / 'images' / split).iterdir() if p.suffix.lower() in ('.jpg', '.png'))
    for index, image in enumerate(images, 1):
        label = root / 'labels' / split / (image.stem + '.txt')
        contents = label.read_bytes()
        rows = light_rows(contents.decode('ascii'), class_id)
        result.append(dict(image=image, label=label, rows=rows, source=dataset, split=split,
                           label_sha256=hashlib.sha256(contents).hexdigest()))
        if index % 5000 == 0:
            print(f'Scanning {dataset}/{split}: {index} labels', flush=True)
    if not result:
        raise ValueError(f'No source labels: {root}/labels/{split}')
    print(f'Scanned {dataset}/{split}: {len(result)} images', flush=True)
    return result


def write_yaml(name, train, val):
    import yaml
    config = dict(path=DATA.as_posix(), train=train, val=val, names={0: 'traffic_light'})
    (DATA / name).write_text(yaml.safe_dump(config, sort_keys=False), encoding='utf-8')


def prepare():
    if DATA.exists():
        raise SystemExit(f'Refusing to overwrite {DATA}. Use --stage check for prepared data.')
    bdd = inventory('bdd8', 'train', 7, bdd_train_selection())
    parts = [('train', 'bdd', bdd),
             ('train', 'bosch', inventory('bosch_light', 'train', 0)),
             ('val', 'bdd', inventory('bdd8', 'val', 7)),
             ('val', 'bosch', inventory('bosch_light', 'val', 0))]
    DATA.mkdir()
    records, stats, smoke = [], {}, {'train': [], 'val': []}
    for split, source, items in parts:
        group = f'{source}_{split}'
        image_dir, label_dir = DATA / 'images' / group, DATA / 'labels' / group
        image_dir.mkdir(parents=True)
        label_dir.mkdir(parents=True)
        count = dict(images=len(items), objects=0, negative_images=0)
        for index, item in enumerate(items):
            image = image_dir / item['image'].name
            label = label_dir / item['label'].name
            # Same-volume NTFS hard links: do not duplicate or rewrite original pixels.
            os.link(item['image'], image)
            contents = '\n'.join(item['rows']) + ('\n' if item['rows'] else '')
            label.write_bytes(contents.encode('ascii'))
            count['objects'] += len(item['rows'])
            count['negative_images'] += not item['rows']
            records.append(dict(image=image.relative_to(DATA).as_posix(),
                                label=label.relative_to(DATA).as_posix(),
                                source_label=str(item['label']), source_sha256=item['label_sha256'],
                                output_sha256=hashlib.sha256(contents.encode('ascii')).hexdigest(),
                                source_class=7 if source == 'bdd' else 0))
            if index and index % 3000 == 0:
                print(f'Prepared {group}: {index}/{len(items)}', flush=True)
        stats[group] = count
        positives = sorted((i for i in items if i['rows']), key=lambda i: order(i['image']))
        negatives = sorted((i for i in items if not i['rows']), key=lambda i: order(i['image']))
        take = 128 if split == 'train' else 32
        for item in positives[:take*3//4] + negatives[:take//4]:
            smoke[split].append((image_dir / item['image'].name).as_posix())
    for split, paths in smoke.items():
        (DATA / f'smoke_{split}.txt').write_text('\n'.join(paths)+'\n', encoding='utf-8')
    write_yaml('traffic_light.yaml', ['images/bdd_train', 'images/bosch_train'],
               ['images/bdd_val', 'images/bosch_val'])
    write_yaml('traffic_light_smoke.yaml', 'smoke_train.txt', 'smoke_val.txt')
    write_yaml('bdd_eval.yaml', 'images/bdd_train', 'images/bdd_val')
    write_yaml('bosch_eval.yaml', 'images/bosch_train', 'images/bosch_val')
    report = dict(version=1, seed='SHA256(filename)', stats=stats, records=records,
                  scope='Single-class location detection. No color or arrow supervision.',
                  bdd_train_selection='12000 positives, size buckets <8/<16/else pixels at 640; 3000 negatives',
                  splits='Original BDD split and Bosch whole-recording split retained; test data unused',
                  image_storage='NTFS hard links; treat both source and linked images as read-only',
                  initial_checkpoint=str(START), initial_checkpoint_sha256=digest(START))
    (DATA / 'manifest.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps(stats, indent=2), flush=True)


def check():
    import yaml
    report = json.loads((DATA / 'manifest.json').read_text(encoding='utf-8'))
    expected_names = {0: 'traffic_light'} if report['version'] == 1 else dict(enumerate(
        ['person', 'rider', 'car', 'bus', 'truck', 'bicycle', 'motorcycle', 'traffic_light']))
    for name in ('traffic_light.yaml', 'traffic_light_smoke.yaml', 'bdd_eval.yaml', 'bosch_eval.yaml'):
        config = yaml.safe_load((DATA / name).read_text(encoding='utf-8'))
        if config['names'] != expected_names or Path(config['path']) != DATA:
            raise ValueError(f'Unexpected dataset configuration: {name}')
    for index, record in enumerate(report['records'], 1):
        label = DATA / record['label']
        if not (DATA / record['image']).is_file():
            raise FileNotFoundError(record['image'])
        source_bytes = Path(record['source_label']).read_bytes()
        output_bytes = label.read_bytes()
        if hashlib.sha256(source_bytes).hexdigest() != record['source_sha256']:
            raise ValueError(f"Source label changed: {record['source_label']}")
        if hashlib.sha256(output_bytes).hexdigest() != record['output_sha256']:
            raise ValueError(f'Output label changed: {label}')
        if record.get('source_kind') == 's2tld_voc':
            from prepare_s2tld import voc_rows
            expected = voc_rows(source_bytes)[0]
        elif report['version'] == 2 and record['supervision'] == 'all_eight':
            expected = source_bytes.decode('ascii').splitlines()
        else:
            expected = light_rows(source_bytes.decode('ascii'), record['source_class'])
            if report['version'] == 2:
                expected = ['7 '+row.split(' ', 1)[1] for row in expected]
        actual = output_bytes.decode('ascii').splitlines()
        if actual != expected:
            raise ValueError(f'Class mapping mismatch: {label}')
        if index % 5000 == 0:
            print(f'Verified {index}/{len(report["records"])} records', flush=True)
    if digest(START) != report['initial_checkpoint_sha256']:
        raise ValueError('Original checkpoint changed since data preparation')
    print(json.dumps(dict(verified_records=len(report['records']), stats=report['stats'],
                          original_labels_and_checkpoint_unchanged=True), indent=2), flush=True)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stage', choices=['prepare', 'add-s2tld', 'check', 'smoke', 'train', 'val'], default='check')
    parser.add_argument('--epochs', type=int, default=30)
    parser.add_argument('--batch', type=int, default=8)
    parser.add_argument('--workers', type=int, default=2)
    parser.add_argument('--model', type=Path, help='Single-class trained checkpoint, required for val')
    args = parser.parse_args()
    if args.epochs < 1 or args.batch < 1 or args.workers < 0:
        parser.error('epochs/batch must be positive, workers must be nonnegative')
    if args.stage == 'prepare':
        prepare()
        check()
        return
    if args.stage == 'add-s2tld':
        from prepare_s2tld import upgrade
        upgrade()
        return
    if args.stage == 'check':
        check()
        return
    if args.stage == 'val' and args.model is None:
        parser.error('--model is required for validation')
    import torch
    from ultralytics import YOLO
    if not torch.cuda.is_available():
        raise SystemExit('CUDA unavailable; use the adas_yolo environment.')
    if not (DATA / 'manifest.json').is_file():
        raise SystemExit('Prepare data first: --stage prepare')
    report = json.loads((DATA / 'manifest.json').read_text(encoding='utf-8'))
    if report['version'] != 2:
        raise SystemExit('Add S2TLD and eight-class partial supervision first: --stage add-s2tld')
    if args.stage == 'val':
        model = YOLO(str(args.model))
        if len(model.names) != 8 or model.names.get(7) != 'traffic_light':
            raise SystemExit('Validation requires the eight-class detector.')
        for source in ('bdd', 'bosch', 's2tld'):
            model.val(data=str(DATA / f'{source}_eval.yaml'), imgsz=640, batch=args.batch,
                      device=0, workers=args.workers, project=str(PROJECT_ROOT / 'runs/traffic_light_eval'),
                      name=source, exist_ok=False, plots=True, classes=None if source == 'bdd' else [7])
        return
    original_sha = digest(START)
    model = YOLO(str(START))
    if model.names.get(7) != 'traffic_light' or len(model.names) != 8:
        raise SystemExit('Unexpected BDD8 checkpoint class mapping.')
    smoke = args.stage == 'smoke'
    options = dict(data=str(DATA / ('traffic_light_smoke.yaml' if smoke else 'traffic_light.yaml')),
                   epochs=1 if smoke else args.epochs, imgsz=640, batch=args.batch, device=0,
                   workers=0 if smoke else args.workers, optimizer='AdamW', lr0=.0002, lrf=.1,
                   warmup_epochs=2., warmup_bias_lr=0., cos_lr=True, patience=10,
                   mosaic=0., scale=.2, close_mosaic=0, mixup=0., cutmix=0., copy_paste=0.,
                   hsv_h=.015, hsv_s=.4, hsv_v=.3,
                   cache=False, amp=True, seed=42, deterministic=True, val=True,
                   cls_remap=True, resume=False, project=str(PROJECT_ROOT / 'runs'),
                   name='traffic_light_v8n_smoke' if smoke else 'traffic_light_v8n_v1_n640', exist_ok=False)
    print(json.dumps(options, indent=2), flush=True)
    import ultralytics
    if ultralytics.__version__ != '8.4.170':
        raise SystemExit('Partial supervision is verified with Ultralytics 8.4.170; use the existing environment.')
    from traffic_light_partial_loss import PartialLightTrainer
    model.train(trainer=PartialLightTrainer, **options)
    if digest(START) != original_sha or len(model.names) != 8 or model.names.get(7) != 'traffic_light':
        raise RuntimeError('Checkpoint preservation or output-class verification failed')
    print(f'Training complete: {model.trainer.save_dir}. Original BDD8 checkpoint unchanged.', flush=True)


if __name__ == '__main__':
    main()  # Windows DataLoader multiprocessing entry point
