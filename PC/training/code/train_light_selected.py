"""Checked, opt-in detector and three-color training. Default --stage check never trains."""
import argparse
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime
import hashlib
import json
from pathlib import Path
import random
import time

import cv2
import numpy as np
import torch
from torch import nn
from torch.utils.data import DataLoader, WeightedRandomSampler
import yaml

from project_paths import PROJECT_ROOT as ROOT, DETECTOR_WEIGHTS
from light_rgb import NAMES, CONTRACT, ColorCrops, build_rgb_model, metrics, reject_uncertain

DATA = ROOT/'datasets/traffic_light_selected_20261019'
VERIFIED = ROOT/'maintenance/light_selection_20261018/verification_final.json'
INITIAL = ROOT/'weights/pretrained/mobilenet_v3_small-047dcff4.pth'
INITIAL_SHA = '047dcff4addef86ea5bc2eff13c9614dc11f47ab1160d0a71a25e7db994f4e1f'
DETECTION_NAMES = ['person', 'rider', 'car', 'bus', 'truck', 'bicycle', 'motorcycle', 'traffic_light']


def sha(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(4*1024*1024), b''):
            h.update(block)
    return h.hexdigest()


def rows(path):
    return [json.loads(line) for line in Path(path).read_text(encoding='utf-8').splitlines()]


def write_json(path, value):
    Path(path).write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding='utf-8')


def verify_file(row):
    image, label = Path(row['image']), Path(row['label'])
    if image.parent.name != row['group'] or label.parent.name != row['group']:
        raise ValueError('Image group and supervision group differ')
    if sha(image) != row['image_sha256'] or sha(label) != row['label_sha256']:
        raise ValueError(f'Data changed after selection: {image}')


def check_data():
    contract = json.loads((ROOT/'maintenance/light_training_20261019/input_contract.json').read_text(encoding='utf-8'))
    for path, expected in contract['files'].items():
        if sha(ROOT/path) != expected:
            raise ValueError(f'Reviewed candidate index changed: {path}')
    reference = json.loads(VERIFIED.read_text(encoding='utf-8'))
    selected = json.loads((DATA/'selection.json').read_text(encoding='utf-8'))
    if selected['detection_candidates'] != reference['detection_candidates']:
        raise ValueError('Selection counts differ from reviewed snapshot')
    original = ROOT/'datasets/traffic_light_v1/manifest.json'
    if sha(original) != reference['source_manifest_sha256']:
        raise ValueError('Original data preparation manifest changed')
    for path, expected in reference['state_index_sha256'].items():
        if sha(path) != expected:
            raise ValueError(f'State index changed: {path}')
    detection = rows(DATA/'detection_candidates.jsonl')
    colors = rows(DATA/'color_candidates.jsonl')
    groups = Counter(r['group'] for r in detection)
    expected = {k: v['images'] for k, v in reference['detection_candidates'].items()}
    if dict(groups) != expected:
        raise ValueError('Candidate groups/counts changed')
    by_image = {r['image']: r for r in detection}
    if len(by_image) != len(detection) or len({r['image_sha256'] for r in detection}) != len(detection):
        raise ValueError('Duplicate selected images, including potential train/val leakage')
    quarantined = {r['image'] for group in reference['duplicate_groups'] for r in group}
    if quarantined.intersection(by_image):
        raise ValueError('Quarantined ambiguous images reintroduced')
    for group in expected:
        listed = (DATA/f'{group}.txt').read_text(encoding='utf-8').splitlines()
        if listed != [r['image'] for r in detection if r['group'] == group]:
            raise ValueError(f'Image list changed: {group}')
    with ThreadPoolExecutor(max_workers=4) as pool:
        for i, _ in enumerate(pool.map(verify_file, detection), 1):
            if i % 4000 == 0:
                print(f'[CHECK] Current image/label SHA256: {i}/{len(detection)}', flush=True)
    color_counts, seen = {}, set()
    from light_color import crop_bounds
    for row in colors:
        parent = by_image.get(row['image'])
        if parent is None or any(row[k] != parent[k] for k in ('source', 'split', 'group', 'size', 'image_sha256')):
            raise ValueError('Color candidate does not match reviewed image')
        if row['color'] not in NAMES:
            raise ValueError('Only explicit red/yellow/green accepted')
        box, (w, h) = row['box'], row['size']
        if min(box[2]-box[0], box[3]-box[1]) < 8 or min(box[:2]) < 0 or box[2] > w or box[3] > h:
            raise ValueError('Color crop violates reviewed bounds/size policy')
        crop_bounds(box, w, h)
        key = row['image'], tuple(box)
        if key in seen:
            raise ValueError('Duplicate or conflicting color crop')
        seen.add(key)
        color_counts.setdefault(row['group'], Counter())[row['color']] += 1
    if color_counts != reference['color_candidates']:
        raise ValueError('Color group counts changed')
    original_record = json.loads(original.read_text(encoding='utf-8'))
    if sha(DETECTOR_WEIGHTS) != original_record['initial_checkpoint_sha256'] or sha(INITIAL) != INITIAL_SHA:
        raise ValueError('Missing detector or incorrect official ImageNet initialization')
    import ultralytics
    if ultralytics.__version__ != '8.4.170':
        raise ValueError('Partial supervision is pinned to the checked Ultralytics 8.4.170 environment')
    from ultralytics import YOLO
    detector = YOLO(str(DETECTOR_WEIGHTS))
    if detector.names != dict(enumerate(DETECTION_NAMES)):
        raise ValueError('Unexpected original detector output classes')
    report = {'stage': 'check', 'training_started': False,
              'detection_counts': dict(groups), 'color_counts': {k: dict(v) for k, v in color_counts.items()},
              'selection_sha256': sha(DATA/'selection.json'),
              'detection_index_sha256': sha(DATA/'detection_candidates.jsonl'),
              'color_index_sha256': sha(DATA/'color_candidates.jsonl'),
              'detector_sha256': sha(DETECTOR_WEIGHTS), 'imagenet_sha256': sha(INITIAL),
              'ultralytics': ultralytics.__version__, 'torch': str(torch.__version__),
              'cuda_available': torch.cuda.is_available(),
              'human_review_complete': False, 'scope': 'Experimental dataset, not guaranteed error-free annotations'}
    return detection, colors, report


def make_configs(folder):
    folder.mkdir()
    names = dict(enumerate(DETECTION_NAMES))
    result = {}
    for source in ('bdd', 'bosch', 's2tld'):
        path = folder/f'{source}.yaml'
        config = {'path': DATA.as_posix(), 'train': (DATA/f'{source}_train.txt').as_posix(),
                  'val': (DATA/f'{source}_val.txt').as_posix(), 'names': names}
        path.write_text(yaml.safe_dump(config, sort_keys=False), encoding='utf-8')
        result[source] = path
    path = folder/'detector.yaml'
    config = {'path': DATA.as_posix(),
              'train': [(DATA/f'{s}_train.txt').as_posix() for s in ('bdd', 'bosch', 's2tld')],
              'val': (DATA/'bdd_val.txt').as_posix(), 'names': names}
    path.write_text(yaml.safe_dump(config, sort_keys=False), encoding='utf-8')
    result['train'] = path
    return result


def train_detector(args, folder, configs):
    from ultralytics import YOLO
    from traffic_light_partial_loss import PartialLightTrainer
    model = YOLO(str(DETECTOR_WEIGHTS))
    model.train(trainer=PartialLightTrainer, data=str(configs['train']), epochs=args.epochs,
                imgsz=640, batch=args.batch, device=0, workers=args.workers,
                optimizer='AdamW', lr0=.0001, lrf=.1, cos_lr=True, warmup_epochs=2.,
                warmup_bias_lr=0., patience=args.patience, scale=.2, mosaic=0., mixup=0., cutmix=0.,
                copy_paste=0., close_mosaic=0, hsv_h=.015, hsv_s=.4, hsv_v=.3,
                cache=False, amp=True, seed=42, deterministic=True, val=True, resume=False,
                cls_remap=True, project=str(folder), name='detector', exist_ok=False)
    actual = Path(model.trainer.save_dir)
    output = {}
    # Paired validation on the SAME filtered data, not comparison with old published AP.
    for tag, path in [('original', DETECTOR_WEIGHTS), ('best', actual/'weights/best.pt'),
                      ('last', actual/'weights/last.pt')]:
        output[tag] = {}
        for source in ('bdd', 'bosch', 's2tld'):
            value = YOLO(str(path)).val(data=str(configs[source]), imgsz=640, batch=args.batch,
                      device=0, workers=args.workers, classes=None if source == 'bdd' else [7],
                      project=str(folder/'detector_comparison'), name=f'{tag}_{source}', exist_ok=False)
            per_class = {}
            for i, class_id in enumerate(value.box.ap_class_index):
                p, r, ap50, ap = value.box.class_result(i)
                per_class[DETECTION_NAMES[int(class_id)]] = dict(precision=float(p), recall=float(r),
                                                               ap50=float(ap50), ap50_95=float(ap))
            output[tag][source] = {'checkpoint': str(path), 'sha256': sha(path), 'per_class': per_class}
    write_json(folder/'detector_comparison.json', output)
    return str(actual/'weights/best.pt')


def seed_worker(_):
    cv2.setNumThreads(1)
    seed = torch.initial_seed() % (2**32)
    random.seed(seed)
    np.random.seed(seed)


def evaluate_color(model, loader, threshold, margin):
    targets, predictions, accepted = [], [], []
    model.eval()
    with torch.inference_mode():
        for images, labels in loader:
            probability = model(images.to(next(model.parameters()).device)).softmax(1).cpu()
            targets.extend(labels.tolist())
            predictions.extend(probability.argmax(1).tolist())
            accepted.extend(reject_uncertain(probability, threshold, margin).tolist())
    return {'argmax': metrics(targets, predictions), 'with_rejection': metrics(targets, accepted)}


def train_color(args, folder, colors, preflight):
    from light_rgb import load_rgb_model
    device = torch.device('cuda:0')
    train = [r for r in colors if r['split'] == 'train']
    validation = [r for r in colors if r['split'] == 'val']
    counts = Counter(r['color'] for r in train)
    # Square-root resampling reduces yellow imbalance without forcing full equal oversampling.
    weights = torch.tensor([1/(counts[r['color']]**.5) for r in train], dtype=torch.double)
    sampler = WeightedRandomSampler(weights, len(train), replacement=True,
                                    generator=torch.Generator().manual_seed(42))
    loader_args = dict(batch_size=args.color_batch, num_workers=args.workers, pin_memory=True,
                       worker_init_fn=seed_worker, generator=torch.Generator().manual_seed(42))
    train_loader = DataLoader(ColorCrops(train, augment=True), sampler=sampler, **loader_args)
    val_loader = DataLoader(ColorCrops(validation), shuffle=False, **loader_args)
    model = build_rgb_model(INITIAL).to(device)
    optimizer = torch.optim.AdamW(model.parameters(), lr=.0003, weight_decay=.0001)
    schedule = torch.optim.lr_scheduler.CosineAnnealingLR(optimizer, T_max=args.color_epochs)
    loss_function = nn.CrossEntropyLoss()
    directory = folder/'color'
    (directory/'weights').mkdir(parents=True)
    write_json(directory/'args.json', dict(vars(args), names=NAMES, preprocessing=CONTRACT,
                                          sampler='square-root inverse class count; original class counts unchanged',
                                          reject_threshold=args.threshold, reject_margin=args.margin))
    history, best, stale = [], -1., 0
    for epoch in range(1, args.color_epochs+1):
        started = time.perf_counter()
        model.train()
        total, count = 0., 0
        for batch_number, (images, labels) in enumerate(train_loader, 1):
            images, labels = images.to(device), labels.to(device)
            optimizer.zero_grad(set_to_none=True)
            loss = loss_function(model(images), labels)
            if not torch.isfinite(loss):
                raise RuntimeError('Non-finite color loss; stopped without publishing a model')
            loss.backward()
            optimizer.step()
            total += float(loss.detach())*len(labels)
            count += len(labels)
            if batch_number % 100 == 0:
                print(f'[COLOR] epoch {epoch}/{args.color_epochs} batch {batch_number}/{len(train_loader)} loss={total/count:.4f}', flush=True)
        result = evaluate_color(model, val_loader, args.threshold, args.margin)
        score = result['argmax']['macro_f1']
        history.append({'epoch': epoch, 'seconds': time.perf_counter()-started,
                        'train_loss': total/max(1, count), 'validation': result})
        checkpoint = {'model_type': 'mobilenet_v3_small_rgb3', 'names': NAMES, 'preprocessing': CONTRACT,
                      'epoch': epoch, 'state_dict': {k: v.detach().cpu().clone() for k, v in model.state_dict().items()},
                      'selection': preflight, 'validation': result,
                      'rejection': {'threshold': args.threshold, 'margin': args.margin,
                                    'note': 'Uncalibrated development defaults; not a trained off/non-signal rejection guarantee'}}
        torch.save(checkpoint, directory/'weights/last.pt')
        if score > best:
            best, stale = score, 0
            torch.save(checkpoint, directory/'weights/best.pt')
        else:
            stale += 1
        write_json(directory/'history.json', history)
        print(f'[COLOR] {epoch}: loss={total/count:.4f} macroF1={score:.4f} coverage={result["with_rejection"]["coverage"]:.4f}', flush=True)
        schedule.step()
        if stale >= args.color_patience:
            print(f'[COLOR] Early stopping: {args.color_patience} epochs without better argmax macro F1.', flush=True)
            break
    evaluation = {}
    for tag in ('best', 'last'):
        model, _ = load_rgb_model(directory/f'weights/{tag}.pt', device)
        evaluation[tag] = {}
        for source in ('bdd', 'bosch', 's2tld'):
            loader = DataLoader(ColorCrops([r for r in validation if r['source'] == source]), shuffle=False, **loader_args)
            evaluation[tag][source] = evaluate_color(model, loader, args.threshold, args.margin)
    write_json(directory/'evaluation.json', {'results': evaluation,
               'scope': 'Ground-truth RGB lamp crops only; not end-to-end detection or off/non-signal accuracy',
               'validation_role': 'Development/checkpoint-selection data, not an independent final test'})
    return str(directory/'weights/best.pt')


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stage', choices=['check', 'detector', 'color', 'all'], default='check')
    parser.add_argument('--epochs', type=int, default=100)
    parser.add_argument('--color-epochs', type=int, default=100)
    parser.add_argument('--patience', type=int, default=15)
    parser.add_argument('--color-patience', type=int, default=10)
    parser.add_argument('--output', help='New run directory; an existing directory is refused')
    parser.add_argument('--batch', type=int, default=8)
    parser.add_argument('--color-batch', type=int, default=64)
    parser.add_argument('--workers', type=int, default=2)
    parser.add_argument('--threshold', type=float, default=.6)
    parser.add_argument('--margin', type=float, default=.15)
    args = parser.parse_args(argv)
    if min(args.epochs, args.color_epochs, args.batch, args.color_batch, args.patience, args.color_patience) < 1 or args.workers < 0:
        parser.error('epochs, patience and batch sizes must be positive; workers nonnegative')
    if not 0 <= args.threshold <= 1 or not 0 <= args.margin <= 1:
        parser.error('rejection threshold/margin must be in [0,1]')
    return args


def main():
    args = parse_args()
    random.seed(42)
    np.random.seed(42)
    torch.manual_seed(42)
    cv2.setNumThreads(1)
    detection, colors, preflight = check_data()
    folder = Path(args.output).resolve() if args.output else ROOT/'runs'/('light_selected_'+datetime.now().strftime('%Y%m%d_%H%M%S_%f'))
    folder.mkdir(parents=True, exist_ok=False)
    write_json(folder/'preflight.json', preflight)
    print(f'CHECK PASSED. Report: {folder}', flush=True)
    if args.stage == 'check':
        print('No training. Execute --stage detector, color, or all to train explicitly.', flush=True)
        return
    if not torch.cuda.is_available():
        raise SystemExit('CUDA unavailable; use the existing adas_yolo environment')
    torch.cuda.manual_seed_all(42)
    configs = make_configs(folder/'configs')
    result = {'stage': args.stage, 'output': str(folder), 'complete': False, 'training_started': True,
              'note': 'Explicit CLI stage selected by operator; preflight metadata describes the earlier check'}
    write_json(folder/'run_status.json', result)
    if args.stage in ('detector', 'all'):
        result['detector_best'] = train_detector(args, folder, configs)
        write_json(folder/'run_status.json', result)
    if args.stage in ('color', 'all'):
        result['color_best'] = train_color(args, folder, colors, preflight)
    if sha(DETECTOR_WEIGHTS) != preflight['detector_sha256'] or sha(INITIAL) != preflight['imagenet_sha256']:
        raise RuntimeError('Original initialization/weights changed unexpectedly')
    result['complete'] = True
    write_json(folder/'run_status.json', result)
    print(json.dumps(result, indent=2), flush=True)


if __name__ == '__main__':
    main()  # Required for Windows DataLoader workers.
