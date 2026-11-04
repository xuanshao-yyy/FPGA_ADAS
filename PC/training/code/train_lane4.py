"""Small/Nano lane4 fine-tuning: check, bounded smoke test and full training."""
import argparse
import csv
from datetime import datetime
import hashlib
import json
import os
from pathlib import Path
import random
import time

import cv2
import numpy as np
import torch
from torch import nn
from torch.utils.data import Dataset, DataLoader

from project_paths import PROJECT_ROOT
from twinlite_lane import load_lane_model, prepare_rgb
from lane4_data import NAMES, IGNORE, overlay_labels


def read_index(root, split):
    return [json.loads(line) for line in (root / f'{split}.jsonl').read_text(encoding='utf-8').splitlines()]


def subset(rows, count, seed):
    if not count or count >= len(rows):
        return rows
    selected = list(rows)
    random.Random(seed).shuffle(selected)
    return selected[:count]


def check_pair_paths(root, rows, split):
    """Index each directory once; retain all-pair checks without per-path stat calls."""
    directory_files = {}
    started = time.perf_counter()
    for index, row in enumerate(rows, 1):
        for key in ['image', 'mask']:
            path = root / row[key]
            directory = path.parent
            if directory not in directory_files:
                print(f'[CHECK] {split}: scanning directory {directory}', flush=True)
                with os.scandir(directory) as entries:
                    names = set()
                    for scanned, entry in enumerate(entries, 1):
                        if entry.is_file():
                            names.add(entry.name)
                        if scanned % 10000 == 0:
                            print(f'[CHECK] {split}: scanned {scanned} directory entries', flush=True)
                directory_files[directory] = names
                print(f'[CHECK] {split}: indexed {len(names)} files', flush=True)
            if path.name not in directory_files[directory]:
                raise RuntimeError(f'Missing dataset pair: {row["stem"]} ({key}: {path})')
        if index % 10000 == 0 or index == len(rows):
            print(f'[CHECK] {split}: paths {index}/{len(rows)} ({time.perf_counter()-started:.1f}s)', flush=True)


def check_dataset(root):
    started = time.perf_counter()
    print(f'[CHECK] Reading dataset configuration and indices: {root}', flush=True)
    cfg = json.loads((root / 'dataset.json').read_text(encoding='utf-8'))
    report = json.loads((root / 'conversion_report.json').read_text(encoding='utf-8'))
    if cfg['classes'] != NAMES or cfg['ignore_index'] != IGNORE or cfg['conversion_errors'] or report['errors']:
        raise RuntimeError('Dataset semantic contract/error check failed')
    train, val = read_index(root, 'train'), read_index(root, 'val')
    print(f'[CHECK] Index loaded: train={len(train)}, val={len(val)}; checking counts and split overlap', flush=True)
    if not train or not val or {r['stem'] for r in train} & {r['stem'] for r in val}:
        raise RuntimeError('Empty/overlapping train and validation split')
    for split, rows in [('train', train), ('val', val)]:
        if len(rows) != report['splits'][split]['supervised_images']:
            raise RuntimeError('Index/report count mismatch')
        if len({r['stem'] for r in rows}) != len(rows):
            raise RuntimeError(f'Duplicate {split} index entries')
        check_pair_paths(root, rows, split)
        samples = subset(rows, 32, 42)
        print(f'[CHECK] {split}: decoding {len(samples)} sampled image/mask pairs', flush=True)
        for row in samples:
            image = cv2.imread(str(root / row['image']))
            target = cv2.imread(str(root / row['mask']), cv2.IMREAD_GRAYSCALE)
            if image is None or target is None or target.shape != (384, 640):
                raise RuntimeError(f'Unreadable dataset pair: {row["stem"]}')
            if image.shape[:2] != (row['height'], row['width']):
                raise RuntimeError('Original image geometry changed')
            if not set(np.unique(target)).issubset({0, 1, 2, 3, IGNORE}):
                raise RuntimeError('Invalid mask class ID')
        print(f'[CHECK] {split}: sampled geometry and class IDs passed', flush=True)
    print(f'[CHECK] Dataset passed in {time.perf_counter()-started:.1f}s', flush=True)
    return cfg, report, train, val


class LaneDataset(Dataset):
    def __init__(self, root, rows, training=False):
        self.root, self.rows, self.training = root, rows, training

    def __len__(self):
        return len(self.rows)

    def __getitem__(self, index):
        row = self.rows[index]
        image = cv2.imread(str(self.root / row['image']))
        target = cv2.imread(str(self.root / row['mask']), cv2.IMREAD_GRAYSCALE)
        if image is None or target is None:
            raise RuntimeError(f'Cannot read pair: {row["stem"]}')
        rgb, _ = prepare_rgb(image)
        if self.training:
            if random.random() < .5:
                rgb, target = np.fliplr(rgb), np.fliplr(target)
            # Preserve geometry; moderate brightness change is applied only to RGB.
            rgb = np.clip(rgb.astype(np.float32) * random.uniform(.85, 1.15), 0, 255).astype(np.uint8)
        return (torch.from_numpy(np.ascontiguousarray(rgb.transpose(2, 0, 1))).float() / 255,
                torch.from_numpy(np.ascontiguousarray(target)).long())


def worker_seed(_):
    seed = torch.initial_seed() % 2**32
    random.seed(seed)
    np.random.seed(seed)
    cv2.setNumThreads(1)


def create_model(device, variant='small'):
    model = load_lane_model(variant, 'cpu')
    from model.model import UpConvBlock
    # Preserve all feature extraction/decoder weights except the two-class final block.
    model.out_ll = UpConvBlock(8, 4, last=True)
    return model.to(device)


def segmentation_loss(logits, target, weights, return_parts=False):
    logits = logits.float()
    ce = nn.functional.cross_entropy(logits, target, weight=weights, ignore_index=IGNORE)
    valid = (target != IGNORE).unsqueeze(1)
    encoded = nn.functional.one_hot(target.clamp(0, 3), 4).permute(0, 3, 1, 2).float() * valid
    probabilities = logits.softmax(1) * valid
    intersection = (probabilities * encoded).sum((0, 2, 3))
    denominator = (probabilities + encoded).sum((0, 2, 3))
    dice = 1 - ((2 * intersection[1:] + 1) / (denominator[1:] + 1)).mean()
    loss = ce + .5 * dice
    return (loss, {'ce': ce.detach(), 'dice': dice.detach()}) if return_parts else loss


def duration(seconds):
    seconds = max(0, int(seconds))
    return f'{seconds//3600:02}:{seconds//60%60:02}:{seconds%60:02}'


def checked_gradient_norm(model, output, epoch, batch, loss):
    try:
        return float(nn.utils.clip_grad_norm_(model.parameters(), 5., error_if_nonfinite=True))
    except RuntimeError as error:
        gradients = {}
        for name, parameter in model.named_parameters():
            if parameter.grad is not None:
                grad = parameter.grad.detach()
                finite = torch.isfinite(grad)
                gradients[name] = dict(nonfinite_values=int((~finite).sum()),
                    finite_max_abs=float(grad[finite].abs().max()) if finite.any() else None)
        diagnostic = dict(epoch=epoch, batch=batch, loss=float(loss.detach()), precision='fp32',
                          error=str(error), gradients=gradients, optimizer_step_performed=False)
        (output/'gradient_failure.json').write_text(json.dumps(diagnostic, indent=2), encoding='utf-8')
        print(f'[ERROR] Gradient check failed; diagnostic: {output/"gradient_failure.json"}', flush=True)
        raise


def metrics_from_confusion(confusion):
    matrix = np.asarray(confusion, np.float64)
    tp = np.diag(matrix)
    union = matrix.sum(0) + matrix.sum(1) - tp
    iou = np.divide(tp, union, out=np.full(4, np.nan), where=union > 0)
    result = {name: float(v) if np.isfinite(v) else None for name, v in zip(NAMES, iou)}
    precision = np.divide(tp, matrix.sum(0), out=np.full(4, np.nan), where=matrix.sum(0) > 0)
    recall = np.divide(tp, matrix.sum(1), out=np.full(4, np.nan), where=matrix.sum(1) > 0)
    return dict(iou=result,
                precision={name: float(v) if np.isfinite(v) else None for name, v in zip(NAMES, precision)},
                recall={name: float(v) if np.isfinite(v) else None for name, v in zip(NAMES, recall)},
                pixel_accuracy=float(tp.sum()/matrix.sum()) if matrix.sum() else None,
                foreground_miou=float(np.nanmean(iou[1:])),
                all_class_miou=float(np.nanmean(iou)), confusion_matrix=matrix.astype(np.int64).tolist(),
                metric_scope='Exact pixel IoU on semantic centerline bands; not box mAP, lane-event accuracy or RK3568 speed')


@torch.inference_mode()
def evaluate(model, loader, weights, device, preview=None):
    model.eval()
    print(f'[VAL] Starting validation: {len(loader.dataset)} images, {len(loader)} batches', flush=True)
    last_progress = time.perf_counter()
    confusion = torch.zeros(16, dtype=torch.int64, device=device)
    total_loss, total_ce, total_dice, batches = 0., 0., 0., 0
    panels = []
    for images, target in loader:
        images, target = images.to(device), target.to(device)
        logits = model(images)
        loss, parts = segmentation_loss(logits, target, weights, return_parts=True)
        if not torch.isfinite(loss):
            raise RuntimeError('Non-finite validation loss')
        prediction = logits.argmax(1)
        valid = target != IGNORE
        confusion += torch.bincount((target[valid]*4 + prediction[valid]).ravel(), minlength=16)
        total_loss += float(loss)
        total_ce += float(parts['ce'])
        total_dice += float(parts['dice'])
        batches += 1
        if time.perf_counter()-last_progress >= 10 or batches == len(loader):
            print(f'[VAL] Batch {batches}/{len(loader)}', flush=True)
            last_progress = time.perf_counter()
        if preview is not None and len(panels) < 4:
            for index in range(min(len(images), 4-len(panels))):
                bgr = cv2.cvtColor((images[index].cpu().numpy().transpose(1, 2, 0)*255).astype(np.uint8), cv2.COLOR_RGB2BGR)
                truth = target[index].cpu().numpy().astype(np.uint8)
                pred = prediction[index].cpu().numpy().astype(np.uint8)
                pred[truth == IGNORE] = IGNORE
                views = [bgr.copy(), overlay_labels(bgr, truth), overlay_labels(bgr, pred)]
                for display, name in zip(views, ['Original', 'BDD labels', 'Prediction']):
                    cv2.putText(display, name, (8, 26), cv2.FONT_HERSHEY_SIMPLEX, .6, (255, 255, 255), 2, cv2.LINE_AA)
                panels.append(np.hstack(views))
    if not batches:
        raise RuntimeError('No validation batches')
    result = metrics_from_confusion(confusion.reshape(4, 4).cpu().numpy())
    result['loss'] = total_loss / batches
    result['ce_loss'], result['dice_loss'] = total_ce / batches, total_dice / batches
    if preview is not None:
        cv2.imwrite(str(preview), np.vstack(panels))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stage', choices=['check', 'smoke', 'train'], default='check')
    parser.add_argument('--variant', choices=['small', 'nano'], default='small')
    parser.add_argument('--output', type=Path, help='Explicit new result directory; existing directories are rejected')
    parser.add_argument('--data', type=Path, default=PROJECT_ROOT / 'datasets/lane4_bdd_v1')
    parser.add_argument('--epochs', type=int)
    parser.add_argument('--batch', type=int)
    parser.add_argument('--workers', type=int)
    parser.add_argument('--device', choices=['cuda:0', 'cpu'], default='cuda:0')
    args = parser.parse_args()
    if any(v is not None and v < 1 for v in [args.epochs, args.batch]) or (args.workers is not None and args.workers < 0):
        parser.error('epochs/batch > 0; workers >= 0')
    root = args.data.resolve()
    cfg, conversion, train_rows, val_rows = check_dataset(root)
    if args.stage == 'train' and not cfg['full_conversion']:
        raise RuntimeError('Full training requires full-conversion data, not a pilot')
    random.seed(cfg['seed'])
    np.random.seed(cfg['seed'])
    torch.manual_seed(cfg['seed'])
    torch.set_num_threads(4)
    cv2.setNumThreads(1)
    if args.stage == 'check':
        print(f'[CHECK] Loading official {args.variant} weights and checking four-class model output on CPU', flush=True)
        model = create_model('cpu', args.variant)
        with torch.inference_mode():
            shape = list(model.eval()(torch.zeros(1, 3, 384, 640)).shape)
        if shape != [1, 4, 384, 640]:
            raise RuntimeError('Model contract mismatch')
        print(json.dumps(dict(dataset=str(root), train_images=len(train_rows), val_images=len(val_rows),
                              variant=args.variant, output_shape=shape, parameters=sum(p.numel() for p in model.parameters()),
                              stage='check only; no training started'), indent=2), flush=True)
        return
    if args.device.startswith('cuda') and not torch.cuda.is_available():
        raise RuntimeError('CUDA is unavailable; select --device cpu for a bounded check')
    smoke = args.stage == 'smoke'
    if smoke:
        train_rows, val_rows = subset(train_rows, 512, cfg['seed']), subset(val_rows, 128, cfg['seed'])
    epochs = args.epochs or (2 if smoke else cfg['epochs'])
    batch = args.batch or cfg['batch']
    workers = cfg['workers'] if args.workers is None else args.workers
    generator = torch.Generator().manual_seed(cfg['seed'])
    train_loader = DataLoader(LaneDataset(root, train_rows, True), batch_size=batch, shuffle=True,
                              num_workers=workers, worker_init_fn=worker_seed, generator=generator,
                              persistent_workers=workers > 0)
    val_loader = DataLoader(LaneDataset(root, val_rows), batch_size=batch, shuffle=False,
                            num_workers=workers, worker_init_fn=worker_seed, persistent_workers=workers > 0)
    device = args.device
    model = create_model(device, args.variant)
    # Derive weights only from training-split pixel counts; cap foreground weights.
    counts = np.asarray(conversion['splits']['train']['class_pixel_counts'], np.float64)
    weights = torch.tensor(np.minimum(np.sqrt(counts[0] / np.maximum(counts, 1)), 10),
                           dtype=torch.float32, device=device)
    pretrained = [p for name, p in model.named_parameters() if not name.startswith('out_ll.')]
    head = list(model.out_ll.parameters())
    optimizer = torch.optim.AdamW([{'params': pretrained, 'lr': cfg['pretrained_lr']},
                                   {'params': head, 'lr': cfg['head_lr']}], weight_decay=cfg['weight_decay'])
    scheduler = torch.optim.lr_scheduler.CosineAnnealingLR(optimizer, T_max=epochs, eta_min=1e-6)
    # FP16 run failed with non-finite gradients. Use FP32 and retain strict gradient checks.
    amp = False
    name = f'lane4_{args.variant}_{"smoke" if smoke else "bdd"}'
    output = args.output.resolve() if args.output else PROJECT_ROOT / 'runs' / name
    if args.output is None:
        suffix = 1
        while output.exists():
            suffix += 1
            output = PROJECT_ROOT / 'runs' / f'{name}-{suffix}'
    output.mkdir(parents=True, exist_ok=False)
    (output / 'weights').mkdir()
    settings = dict(dataset=str(root), dataset_config_sha256=hashlib.sha256((root/'dataset.json').read_bytes()).hexdigest(),
                    official_weights_sha256=hashlib.sha256((PROJECT_ROOT/f'weights/twinlitenetplus/{args.variant}.pth').read_bytes()).hexdigest(),
                    variant=args.variant, stage=args.stage, epochs=epochs, batch=batch, workers=workers, device=device,
                    train_images=len(train_rows), val_images=len(val_rows), seed=cfg['seed'],
                    train_index_sha256=hashlib.sha256((root/'train.jsonl').read_bytes()).hexdigest(),
                    val_index_sha256=hashlib.sha256((root/'val.jsonl').read_bytes()).hexdigest(),
                    class_weights=weights.cpu().tolist(), pretrained_lr=cfg['pretrained_lr'], head_lr=cfg['head_lr'],
                    loss='weighted CE + 0.5 foreground soft Dice', pretrained_bn='frozen running statistics',
                    optimizer='AdamW', betas=[.9, .999], weight_decay=cfg['weight_decay'],
                    input_shape=[3, 384, 640], parameters=sum(p.numel() for p in model.parameters()),
                    precision='fp32', gradient_clip_max_norm=5., patience=cfg['patience'],
                    scheduler='CosineAnnealingLR', min_lr=1e-6,
                    no_lane_annotation_policy=conversion['no_lane_annotation_policy'], classes=NAMES,
                    amp=amp, board_tested=False, model_finalized=False)
    (output / 'args.json').write_text(json.dumps(settings, indent=2), encoding='utf-8')
    print('[TRAIN] Settings:', json.dumps(settings), flush=True)
    print(f'[TRAIN] Output: {output}; running initial validation before training', flush=True)
    initial_start = time.perf_counter()
    initial = evaluate(model, val_loader, weights, device, output/'val_initial.jpg')
    initial_validation_seconds = time.perf_counter()-initial_start
    (output / 'initial_validation.json').write_text(json.dumps(initial, indent=2), encoding='utf-8')
    best, stale, history = -1., 0, []
    started = time.perf_counter()
    with (output/'results.csv').open('x', newline='', encoding='utf-8') as csvfile:
        fields = ['epoch', 'started_at', 'ended_at', 'train_loss', 'train_ce_loss', 'train_dice_loss',
                  'val_loss', 'val_ce_loss', 'val_dice_loss', 'foreground_miou',
                  'solid_iou', 'dashed_iou', 'crosswalk_iou',
                  'solid_precision', 'solid_recall', 'dashed_precision', 'dashed_recall',
                  'crosswalk_precision', 'crosswalk_recall', 'pixel_accuracy',
                  'pretrained_lr', 'head_lr', 'gradient_norm_mean', 'gradient_norm_max',
                  'optimizer_updates', 'gpu_peak_allocated_mib', 'gpu_peak_reserved_mib',
                  'train_seconds', 'validation_seconds', 'epoch_seconds', 'elapsed_seconds',
                  'remaining_at_max_epochs_seconds', 'best_foreground_miou', 'no_improvement_epochs']
        writer = csv.DictWriter(csvfile, fieldnames=fields)
        writer.writeheader()
        csvfile.flush()
        for epoch in range(1, epochs+1):
            epoch_start = time.perf_counter()
            started_at = datetime.now().astimezone().isoformat(timespec='seconds')
            pretrained_lr, head_lr = [group['lr'] for group in optimizer.param_groups]
            if device.startswith('cuda'):
                torch.cuda.reset_peak_memory_stats(device)
            print(f'[EPOCH {epoch}/{epochs}] {args.variant} | FP32 | batch={batch} | '
                  f'lr_pretrained={pretrained_lr:.7g} lr_head={head_lr:.7g}', flush=True)
            model.train()
            for module_name, module in model.named_modules():
                if isinstance(module, nn.BatchNorm2d) and not module_name.startswith('out_ll.'):
                    module.eval()
            total_loss, total_ce, total_dice, norm_sum, norm_max = 0., 0., 0., 0., 0.
            for index, (images, target) in enumerate(train_loader):
                images, target = images.to(device), target.to(device)
                optimizer.zero_grad(set_to_none=True)
                logits = model(images)
                loss, parts = segmentation_loss(logits, target, weights, return_parts=True)
                if not torch.isfinite(loss):
                    raise RuntimeError(f'Non-finite training loss epoch {epoch} batch {index}')
                loss.backward()
                norm = checked_gradient_norm(model, output, epoch, index+1, loss)
                optimizer.step()
                total_loss += float(loss.detach())
                total_ce += float(parts['ce'])
                total_dice += float(parts['dice'])
                norm_sum, norm_max = norm_sum + norm, max(norm_max, norm)
                if (index+1) % 20 == 0 or index+1 == len(train_loader):
                    elapsed = time.perf_counter()-epoch_start
                    eta = elapsed/(index+1)*(len(train_loader)-index-1)
                    print(f'EPOCH {epoch}/{epochs} BATCH {index+1}/{len(train_loader)} '
                          f'loss={float(loss.detach()):.4f} avg={total_loss/(index+1):.4f} '
                          f'grad={norm:.3f} elapsed={duration(elapsed)} train_eta={duration(eta)}', flush=True)
            train_seconds = time.perf_counter()-epoch_start
            validation_start = time.perf_counter()
            validation = evaluate(model, val_loader, weights, device, output/'val_latest.jpg')
            validation_seconds = time.perf_counter()-validation_start
            improved = validation['foreground_miou'] > best
            best, stale = (validation['foreground_miou'], 0) if improved else (best, stale+1)
            row = dict(epoch=epoch, started_at=started_at, train_loss=total_loss/len(train_loader),
                       train_ce_loss=total_ce/len(train_loader), train_dice_loss=total_dice/len(train_loader),
                       val_loss=validation['loss'], val_ce_loss=validation['ce_loss'], val_dice_loss=validation['dice_loss'],
                       foreground_miou=validation['foreground_miou'],
                       solid_iou=validation['iou']['solid'], dashed_iou=validation['iou']['dashed'],
                       crosswalk_iou=validation['iou']['crosswalk'], pixel_accuracy=validation['pixel_accuracy'],
                       pretrained_lr=pretrained_lr, head_lr=head_lr, optimizer_updates=len(train_loader),
                       gradient_norm_mean=norm_sum/len(train_loader), gradient_norm_max=norm_max,
                       train_seconds=train_seconds, validation_seconds=validation_seconds,
                       best_foreground_miou=best, no_improvement_epochs=stale,
                       gpu_peak_allocated_mib=torch.cuda.max_memory_allocated(device)/2**20 if device.startswith('cuda') else 0.,
                       gpu_peak_reserved_mib=torch.cuda.max_memory_reserved(device)/2**20 if device.startswith('cuda') else 0.)
            for cls in ['solid', 'dashed', 'crosswalk']:
                for metric in ['precision', 'recall']:
                    row[f'{cls}_{metric}'] = validation[metric][cls]
            scheduler.step()
            checkpoint = dict(state_dict=model.state_dict(), optimizer=optimizer.state_dict(),
                              scaler={}, scheduler=scheduler.state_dict(), epoch=epoch, classes=NAMES, variant=args.variant,
                              preprocessing='RGB /255,640x384 letterbox114', settings=settings,
                              metrics=validation, smoke=smoke)
            torch.save(checkpoint, output/'weights/last.pt')
            if improved:
                torch.save(checkpoint, output/'weights/best.pt')
            (output/'validation.json').write_text(json.dumps(validation, indent=2), encoding='utf-8')
            row['ended_at'] = datetime.now().astimezone().isoformat(timespec='seconds')
            row['epoch_seconds'] = time.perf_counter()-epoch_start
            row['elapsed_seconds'] = time.perf_counter()-started
            row['remaining_at_max_epochs_seconds'] = (sum(h['epoch_seconds'] for h in history)+row['epoch_seconds'])/(len(history)+1)*(epochs-epoch)
            writer.writerow(row)
            csvfile.flush()
            history.append(row)
            print(f'[EPOCH {epoch}/{epochs} DONE] train={duration(train_seconds)} '
                  f'val={duration(validation_seconds)} total={duration(row["epoch_seconds"])} '
                  f'mIoU={best:.4f} (best) peak_GPU={row["gpu_peak_allocated_mib"]:.0f}MiB '
                  f'max_epochs_eta={duration(row["remaining_at_max_epochs_seconds"])}', flush=True)
            print('VALIDATION', json.dumps(row), flush=True)
            if stale >= cfg['patience']:
                print(f'[EARLY STOP] No foreground mIoU improvement for {stale} epochs', flush=True)
                break
    saved = torch.load(output/'weights/best.pt', map_location='cpu', weights_only=True)
    reloaded = create_model('cpu', args.variant)
    reloaded.load_state_dict(saved['state_dict'], strict=True)
    report = dict(completed_epochs=len(history), wall_seconds=time.perf_counter()-started,
                  initial=initial, last=validation, best_foreground_miou=best,
                  initial_validation_seconds=initial_validation_seconds,
                  checkpoint_reload_passed=True, variant=args.variant, precision='fp32', stage=args.stage, model_finalized=False,
                  board_tested=False, warnings=['Smoke only verifies optimization and IO; it does not establish usable accuracy'] if smoke else [],
                  history=history)
    (output/'report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print('COMPLETE', output, flush=True)


if __name__ == '__main__':
    main()
