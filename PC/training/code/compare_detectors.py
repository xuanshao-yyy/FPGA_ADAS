"""Compare local eight-class detectors on identical road-video frames."""
import argparse
import csv
import hashlib
import json
import time
from collections import Counter
from pathlib import Path

import cv2
import numpy as np
import torch
import ultralytics
from ultralytics import YOLO
from ultralytics.utils.files import increment_path

from project_paths import PROJECT_ROOT

NAMES = ['person', 'rider', 'car', 'bus', 'truck', 'bicycle', 'motorcycle', 'traffic_light']
SPECS = [
    ('v8_original', 'A: YOLOv8n original best', 'bdd8_full_n640', 'best.pt', True),
    ('v8_light_best', 'B: YOLOv8n light best', 'traffic_light_v8n_v1_n640', 'best.pt', True),
    ('v8_light_last', 'C: YOLOv8n light last', 'traffic_light_v8n_v1_n640', 'last.pt', True),
    ('yolo26_e2e', 'D: YOLO26n end-to-end', 'bdd8_yolo26n_full_n640', 'best.pt', False),
    ('yolo26_nms', 'E: YOLO26n NMS', 'bdd8_yolo26n_full_n640', 'best.pt', True),
]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def tile(image, heading, size=(640, 360)):
    image = cv2.resize(image, size)
    image = cv2.copyMakeBorder(image, 36, 0, 0, 0, cv2.BORDER_CONSTANT)
    cv2.putText(image, heading, (10, 25), cv2.FONT_HERSHEY_SIMPLEX, .6,
                (255, 255, 255), 1, cv2.LINE_AA)
    return image


def grid(images, headings, size=(640, 360)):
    tiles = [tile(image, title, size) for image, title in zip(images, headings)]
    return np.vstack([np.hstack(tiles[:3]), np.hstack(tiles[3:])])


def writer(path, fps, shape):
    out = cv2.VideoWriter(str(path), cv2.CAP_MSMF, cv2.VideoWriter_fourcc(*'H264'), fps, shape)
    if not out.isOpened():
        raise RuntimeError(f'Cannot encode {path}')
    return out


def verify_video(path, frames, fps, shape):
    cap = cv2.VideoCapture(str(path))
    try:
        assert cap.isOpened(), path
        assert int(cap.get(cv2.CAP_PROP_FRAME_COUNT)) == frames, path
        assert abs(cap.get(cv2.CAP_PROP_FPS) - fps) < .01, path
        for index in sorted({0, frames // 2, frames - 1}):
            cap.set(cv2.CAP_PROP_POS_FRAMES, index)
            ok, image = cap.read()
            assert ok and image.shape[:2] == (shape[1], shape[0]), (path, index)
    finally:
        cap.release()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, nargs='+', help='Video files; defaults to six road clips')
    parser.add_argument('--output', type=Path, default=PROJECT_ROOT / 'runs/detector_comparison_20261004')
    parser.add_argument('--frames', type=int, help='Optional per-video frame limit for execution checks')
    args = parser.parse_args()
    if args.frames is not None and args.frames < 1:
        parser.error('--frames must be positive')
    videos = sorted(p.resolve() for p in args.source) if args.source else sorted(
        p for p in (PROJECT_ROOT / 'video').glob('*.mp4')
        if p.name not in ('video_20260929_111440.mp4', 'video_20260929_112739.mp4'))
    if not videos or any(not p.is_file() for p in videos) or len({p.stem for p in videos}) != len(videos):
        parser.error('Provide existing videos with unique stems')
    if not torch.cuda.is_available():
        raise SystemExit('Use the existing CUDA-enabled adas_yolo environment')
    output = increment_path(args.output.resolve(), mkdir=True)
    settings = dict(imgsz=640, conf=.25, iou=.7, max_det=300, device=0,
                    rect=True, quantize=None, augment=False, save=False, verbose=False)
    report = {'output': str(output), 'settings': settings, 'tracking': False,
              'environment': {'ultralytics': ultralytics.__version__, 'torch': torch.__version__,
                              'gpu': torch.cuda.get_device_name(0)},
              'note': 'Five configurations from four checkpoints. Prediction counts are not accuracy. '
                      'End-to-end branch does not apply conventional IoU NMS. PC timing is not RK3568 timing.',
              'models': {}, 'videos': []}
    models = {}
    for key, label, run, filename, nms in SPECS:
        checkpoint = PROJECT_ROOT / 'runs' / run / 'weights' / filename
        model = YOLO(str(checkpoint))
        if [model.names[k] for k in range(8)] != NAMES:
            raise RuntimeError(f'Unexpected class mapping: {checkpoint}')
        for _ in range(3):
            model.predict(np.zeros((1080, 1920, 3), dtype=np.uint8), nms=nms, **settings)
        end2end = bool(model.predictor.model.end2end)
        assert end2end == (not nms), (key, end2end)
        models[key] = model
        report['models'][key] = {'label': label, 'checkpoint': str(checkpoint),
                                'sha256': sha(checkpoint), 'nms': nms, 'actual_end2end': end2end,
                                'names': model.names}
        print(f'LOADED {key} actual_end2end={end2end}', flush=True)
    (output / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    columns = ['video', 'frame', 'seconds', 'model', 'inference_ms', *NAMES]
    with (output / 'frame_counts.csv').open('w', encoding='utf-8', newline='') as count_file, \
            (output / 'sample_detections.jsonl').open('w', encoding='utf-8') as boxes_file:
        counts_writer = csv.DictWriter(count_file, fieldnames=columns)
        counts_writer.writeheader()
        for source in videos:
            folder = output / source.stem
            folder.mkdir()
            samples = folder / 'samples'
            samples.mkdir()
            cap = cv2.VideoCapture(str(source))
            if not cap.isOpened():
                raise RuntimeError(f'Cannot decode {source}')
            fps = cap.get(cv2.CAP_PROP_FPS)
            source_frames = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
            limit = min(source_frames, args.frames) if args.frames else source_frames
            if fps <= 0 or limit < 1:
                raise RuntimeError(f'Invalid video metadata: {source}')
            sample_indices = sorted({0, limit // 4, limit // 2, 3 * limit // 4, limit - 1})
            info = {'source': str(source), 'fps': fps, 'source_frames': source_frames,
                    'sample_indices': sample_indices, 'comparison': str(folder / 'comparison.mp4'),
                    'individual_videos': {}, 'models': {key: {'counts': Counter(), 'timing': []} for key in models}}
            writers = {}
            processed = 0
            begin = time.perf_counter()
            print(f'START {source.name} frames={limit}', flush=True)
            try:
                writers['comparison'] = writer(folder / 'comparison.mp4', fps, (1920, 792))
                for key in models:
                    destination = folder / f'{key}.mp4'
                    writers[key] = writer(destination, fps, (1280, 720))
                    info['individual_videos'][key] = str(destination)
                for index in range(limit):
                    ok, frame = cap.read()
                    if not ok:
                        raise RuntimeError(f'Cannot read {source} frame {index}')
                    images = [frame]
                    titles = [f'RAW | {source.stem[-6:]} | {index / fps:.2f}s']
                    for key, label, _, _, nms in SPECS:
                        result = models[key].predict(frame, nms=nms, **settings)[0]
                        annotated = result.plot(line_width=2, font_size=10)
                        images.append(annotated)
                        titles.append(label)
                        writers[key].write(cv2.resize(annotated, (1280, 720)))
                        counts = Counter(result.names[int(c)] for c in result.boxes.cls.tolist())
                        inference_ms = result.speed['inference']
                        info['models'][key]['counts'].update(counts)
                        info['models'][key]['timing'].append(inference_ms)
                        counts_writer.writerow({'video': source.name, 'frame': index, 'seconds': index / fps,
                                                'model': key, 'inference_ms': inference_ms,
                                                **{name: counts[name] for name in NAMES}})
                        if index in sample_indices:
                            cv2.imwrite(str(samples / f'{index:05d}_{key}.jpg'), annotated)
                            boxes_file.write(json.dumps({'video': source.name, 'frame': index, 'model': key,
                                'boxes': result.boxes.data.cpu().tolist()}) + '\n')
                    comparison = grid(images, titles)
                    writers['comparison'].write(comparison)
                    if index in sample_indices:
                        cv2.imwrite(str(samples / f'{index:05d}_comparison.jpg'), grid(images, titles, (960, 540)))
                        cv2.imwrite(str(samples / f'{index:05d}_raw.jpg'), frame)
                    processed += 1
                    if processed % 300 == 0:
                        print(f'PROGRESS {source.name} {processed}/{limit}', flush=True)
            finally:
                cap.release()
                for out in writers.values():
                    out.release()
            assert processed == limit
            verify_video(folder / 'comparison.mp4', processed, fps, (1920, 792))
            for key, destination in info['individual_videos'].items():
                verify_video(Path(destination), processed, fps, (1280, 720))
            for values in info['models'].values():
                timing = values.pop('timing')
                values['mean_inference_ms'] = float(np.mean(timing))
                values['p95_inference_ms'] = float(np.percentile(timing, 95))
            info.update(processed_frames=processed, processing_seconds=time.perf_counter() - begin,
                        output_videos_verified=True)
            report['videos'].append(info)
            (output / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
            print(f'DONE {source.name} all 6 outputs verified', flush=True)
    assert all(sha(Path(v['checkpoint'])) == v['sha256'] for v in report['models'].values())
    report['weights_unchanged'] = True
    report['total_source_frames'] = sum(v['processed_frames'] for v in report['videos'])
    (output / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    print(f'COMPLETE {output}', flush=True)


if __name__ == '__main__':
    main()
