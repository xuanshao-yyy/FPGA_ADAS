"""Audit saved best metrics and compare trained Small/Nano on BDD samples and road clips."""
import csv
import hashlib
import json
import math
from pathlib import Path
import random
import time

import cv2
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np
import torch

from project_paths import PROJECT_ROOT
from run_lane4 import load_checkpoint
from twinlite_lane import prepare_rgb, prepare_tensor
from lane4_data import NAMES, IGNORE, overlay_labels
from train_lane4 import metrics_from_confusion, read_index

RUNS = {'small': 'lane4_small_bdd-5', 'nano': 'lane4_nano_bdd'}
CLIPS = [('112454', 0), ('112507', 0), ('112739', 0), ('113343', 0),
         ('113421', 0), ('113452', 0), ('113539', 35)]


def label(panel, title):
    view = panel.copy()
    cv2.rectangle(view, (0, 0), (view.shape[1], 32), (20, 20, 20), -1)
    cv2.putText(view, title, (8, 23), cv2.FONT_HERSHEY_SIMPLEX, .55, (255, 255, 255), 1, cv2.LINE_AA)
    return view


def infer(models, image, device, content_only=False):
    rgb, (left, top, width, height) = prepare_rgb(image)
    tensor = prepare_tensor(rgb).to(device)
    predictions, times = {}, {}
    for variant, model in models.items():
        if device.startswith('cuda'):
            torch.cuda.synchronize()
        started = time.perf_counter()
        logits = model(tensor)
        if device.startswith('cuda'):
            torch.cuda.synchronize()
        times[variant] = (time.perf_counter()-started)*1000
        logits = logits.cpu().numpy()
        if logits.shape != (1, 4, 384, 640) or not np.isfinite(logits).all():
            raise RuntimeError(f'Invalid {variant} output')
        if content_only:
            logits = np.stack([cv2.resize(channel[top:top+height, left:left+width],
                                         (image.shape[1], image.shape[0])) for channel in logits[0]])
        else:
            logits = logits[0]
        predictions[variant] = logits.argmax(0).astype(np.uint8)
    return predictions, times, rgb


def audit_training(device):
    models, reports, curves = {}, {}, {}
    data = PROJECT_ROOT/'datasets/lane4_bdd_v1'
    for variant, name in RUNS.items():
        run = PROJECT_ROOT/'runs'/name
        completed = json.loads((run/'report.json').read_text(encoding='utf-8'))
        if completed['completed_epochs'] != 30 or not completed['checkpoint_reload_passed']:
            raise RuntimeError(f'Incomplete training: {run}')
        rows = list(csv.DictReader((run/'results.csv').open(encoding='utf-8')))
        best = max(rows, key=lambda row: float(row['foreground_miou']))
        checkpoint = run/'weights/best.pt'
        model, saved = load_checkpoint(checkpoint, device)
        settings = saved['settings']
        for relative, key in [('dataset.json', 'dataset_config_sha256'), ('train.jsonl', 'train_index_sha256'),
                              ('val.jsonl', 'val_index_sha256')]:
            if hashlib.sha256((data/relative).read_bytes()).hexdigest() != settings[key]:
                raise RuntimeError(f'Training data changed: {relative}')
        recomputed = metrics_from_confusion(saved['metrics']['confusion_matrix'])
        if saved['epoch'] != int(best['epoch']) or saved['variant'] != variant or saved['smoke']:
            raise RuntimeError('Best checkpoint provenance mismatch')
        if not math.isclose(recomputed['foreground_miou'], float(best['foreground_miou']), abs_tol=1e-12):
            raise RuntimeError('Saved confusion matrix does not reproduce best validation mIoU')
        reports[variant] = dict(checkpoint=str(checkpoint), sha256=hashlib.sha256(checkpoint.read_bytes()).hexdigest(),
            best_epoch=saved['epoch'], best_metrics=recomputed, best_val_loss=saved['metrics']['loss'],
            last_foreground_miou=completed['last']['foreground_miou'],
            train_images=settings['train_images'], val_images=settings['val_images'],
            parameters=sum(p.numel() for p in model.parameters()),
            total_epoch_seconds=sum(float(row['epoch_seconds']) for row in rows),
            checkpoint_reloaded=True, metric_recomputed_from_saved_confusion=True)
        models[variant], curves[variant] = model, rows
    return models, reports, curves, data


def plot_curves(curves, output):
    fig, axes = plt.subplots(2, 2, figsize=(12, 8))
    for variant, rows in curves.items():
        epochs = [int(r['epoch']) for r in rows]
        for key in ['train_loss', 'val_loss']:
            axes[0, 0].plot(epochs, [float(r[key]) for r in rows], label=f'{variant} {key}')
        axes[0, 1].plot(epochs, [float(r['foreground_miou'])*100 for r in rows], label=variant)
        for cls, color in [('solid', 'green'), ('dashed', 'royalblue'), ('crosswalk', 'purple')]:
            axes[1, 0].plot(epochs, [float(r[f'{cls}_iou'])*100 for r in rows],
                           color=color, linestyle='-' if variant == 'small' else '--', label=f'{variant} {cls}')
        axes[1, 1].plot(epochs, [float(r['epoch_seconds'])/60 for r in rows], label=variant)
    for ax, title, ylabel in zip(axes.flat, ['Train/validation loss', 'Foreground mean IoU', 'Class IoU', 'Epoch duration'],
                                ['Loss', 'Percent', 'Percent', 'Minutes']):
        ax.set(title=title, xlabel='Epoch', ylabel=ylabel)
        ax.grid(alpha=.25)
        ax.legend(fontsize=8)
    fig.suptitle('BDD semantic centerline bands; validation pixels, not driving-event accuracy')
    fig.tight_layout()
    fig.savefig(output/'training_curves.png', dpi=150)
    plt.close(fig)


def validation_samples(models, data, device, output):
    rows = read_index(data, 'val')
    random.Random(42).shuffle(rows)
    predicates = [('night', lambda r: r['timeofday'] == 'night'),
                  ('dawn', lambda r: r['timeofday'] == 'dawn/dusk'),
                  ('crosswalk', lambda r: 3 in r['classes']),
                  ('dashed', lambda r: 2 in r['classes']),
                  ('solid', lambda r: 1 in r['classes']),
                  ('negative', lambda r: not r['classes'])]
    chosen, used = [], set()
    for group, predicate in predicates:
        matching = [r for r in rows if r['stem'] not in used and predicate(r)][:4]
        chosen.extend((group, row) for row in matching)
        used.update(r['stem'] for r in matching)
    records, displays = [], []
    for group, row in chosen:
        image = cv2.imread(str(data/row['image']))
        truth = cv2.imread(str(data/row['mask']), cv2.IMREAD_GRAYSCALE)
        if image is None or truth is None:
            raise RuntimeError('Missing validation sample')
        predictions, _, rgb = infer(models, image, device)
        original = cv2.cvtColor(rgb, cv2.COLOR_RGB2BGR)
        panels = [label(original, f'{group} | {row["stem"]}'), label(overlay_labels(original, truth), 'BDD labels')]
        score = {}
        for variant in models:
            pred = predictions[variant]
            valid = truth != IGNORE
            confusion = np.bincount(truth[valid].astype(np.int64)*4+pred[valid], minlength=16).reshape(4, 4)
            # Negative-only samples report false foreground pixels, not undefined foreground mIoU.
            score[variant] = dict(foreground_pixels=int(((pred > 0) & valid).sum()),
                                 iou=metrics_from_confusion(confusion)['iou'] if truth[valid].max() > 0 else None)
            pred[~valid] = IGNORE
            panels.append(label(overlay_labels(original, pred), variant))
        combined = np.hstack(panels)
        cv2.imwrite(str(output/f'validation_{len(records):02}_{row["stem"]}.jpg'), combined)
        displays.append(cv2.resize(combined, (1280, 192)))
        records.append(dict(group=group, source=row, sample_metrics=score))
    for i in range(0, len(displays), 6):
        cv2.imwrite(str(output/f'validation_overview_{i//6+1}.jpg'), np.vstack(displays[i:i+6]))
    return records


def road_clips(models, training, device, output):
    reports = []
    for suffix, seconds_start in CLIPS:
        source = PROJECT_ROOT/f'video/video_20260929_{suffix}.mp4'
        capture = cv2.VideoCapture(str(source))
        if not capture.isOpened():
            raise RuntimeError(f'Cannot read road video: {source}')
        fps, total = capture.get(cv2.CAP_PROP_FPS), int(capture.get(cv2.CAP_PROP_FRAME_COUNT))
        begin, end = round(seconds_start*fps), min(total, round((seconds_start+12)*fps))
        capture.set(cv2.CAP_PROP_POS_FRAMES, begin)
        destination = output/f'{source.stem}_comparison.mp4'
        writer = cv2.VideoWriter(str(destination), cv2.CAP_MSMF, cv2.VideoWriter_fourcc(*'H264'), fps, (1920, 360))
        if not writer.isOpened() or begin >= end:
            raise RuntimeError('Cannot create comparison clip')
        selected = {max(0, (end-begin)*n//4) for n in [1, 2, 3]}
        frames, previews = 0, []
        times = {v: [] for v in models}
        try:
            while frames < end-begin:
                ok, image = capture.read()
                if not ok:
                    break
                view = cv2.resize(image, (640, 360))
                predictions, timing, _ = infer(models, view, device, content_only=True)
                panels = [label(view, f'Original | {suffix} | {(begin+frames)/fps:.2f}s')]
                for variant in models:
                    display = label(overlay_labels(view, predictions[variant]),
                                    f'{variant} | trained best, epoch {training[variant]["best_epoch"]}')
                    cv2.putText(display, 'Green solid | Blue dashed | Magenta crosswalk', (8, 346),
                                cv2.FONT_HERSHEY_SIMPLEX, .42, (255, 255, 255), 1)
                    panels.append(display)
                    times[variant].append(timing[variant])
                comparison = np.hstack(panels)
                writer.write(comparison)
                if frames in selected:
                    previews.append(comparison)
                    cv2.imwrite(str(output/f'{suffix}_frame_{begin+frames}.jpg'), comparison)
                frames += 1
                if frames % 120 == 0:
                    print('VIDEO', suffix, frames, '/', end-begin, flush=True)
        finally:
            capture.release()
            writer.release()
        check = cv2.VideoCapture(str(destination))
        decoded = 0
        while True:
            ok, _ = check.read()
            if not ok:
                break
            decoded += 1
        check.release()
        if frames != end-begin or decoded != frames:
            raise RuntimeError(f'Saved video frame integrity failed: {suffix}')
        cv2.imwrite(str(output/f'{suffix}_overview.jpg'), np.vstack(previews))
        reports.append(dict(source=str(source), start_frame=begin, frames=frames, fps=fps,
            output=destination.name, all_saved_frames_decoded=True, video_ground_truth_available=False,
            pc_forward_ms={v:dict(mean=float(np.mean(t)), p95=float(np.percentile(t,95))) for v,t in times.items()}))
    return reports


def main():
    torch.set_num_threads(4)
    cv2.setNumThreads(2)
    device = 'cuda:0' if torch.cuda.is_available() else 'cpu'
    output = PROJECT_ROOT/'runs/lane4_evaluation_20261005'
    number = 1
    while output.exists():
        number += 1
        output = PROJECT_ROOT/f'runs/lane4_evaluation_20261005-{number}'
    output.mkdir()
    models, training, curves, data = audit_training(device)
    plot_curves(curves, output)
    with torch.inference_mode():
        for model in models.values():
            for _ in range(5):
                model(torch.zeros(1, 3, 384, 640, device=device))
        samples = validation_samples(models, data, device, output)
        clips = road_clips(models, training, device, output)
    report = dict(training=training, device=device, classes=NAMES, validation_sample_count=len(samples),
        validation_samples=samples, road_clips=clips, board_tested=False, driving_event_accuracy_measured=False,
        timing_scope='PC synchronized model forward; excludes decode, transfer, drawing, encoding; not RK3568 FPS',
        limitations=['Centerline-band labels, not physical paint masks', 'Road clips have no ground truth',
                     'No ego-lane selection, curve fitting, temporal smoothing or driving decisions applied'])
    (output/'report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print('COMPLETE', output, flush=True)


if __name__ == '__main__':
    main()
