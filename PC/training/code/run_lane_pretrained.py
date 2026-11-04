"""Run pretrained UFLD-v2 on videos, comparing raw vs temporal lane curves.

This is inference, not training. Lane existence counts are NOT accuracy.
Outputs are incremented; existing video and trained object weights are preserved.
"""
import argparse
from collections import Counter
import json
from pathlib import Path
from project_paths import PROJECT_ROOT, DETECTOR_WEIGHTS
import time

import cv2
import numpy as np
import torch
from ultralytics import YOLO
from ultralytics.utils.files import increment_path

from ufld_lane import UFLDLaneDetector, LaneCurveFilter, draw_curves, lane_offset
from road_tracker import RoadTracker
from track_videos import draw_tracks


def main():
    root = PROJECT_ROOT
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', type=Path, default=root / 'video/video_20260929_113539.mp4')
    parser.add_argument('--start', type=float, default=35)
    parser.add_argument('--seconds', type=float, default=12)
    parser.add_argument('--all-roads', action='store_true', help='Process all 7 real-road videos, full duration')
    parser.add_argument('--show', action='store_true')
    args = parser.parse_args()
    if args.start < 0 or args.seconds <= 0: parser.error('start >= 0, seconds > 0')
    cv2.setNumThreads(4)
    torch.set_num_threads(4)
    output = increment_path(root / 'runs/lane_pretrained', mkdir=True)
    detector = UFLDLaneDetector()
    objects = YOLO(str(DETECTOR_WEIGHTS))
    paths = sorted(p for p in (root / 'video').glob('*.mp4') if '111440' not in p.name) if args.all_roads else [args.source.resolve()]
    summary = dict(model='UFLD-v2 CULane ResNet18 official pretrained',
        parameters=detector.parameters, pretrained_source=str(root / 'weights/ufld_v2/model_source.json'),
        trained_this_run=False, lane_type_available=False, rknn_validated=False,
        camera_calibrated=False, ground_truth_available=False, videos=[])
    for path in paths:
        cap = cv2.VideoCapture(str(path))
        if not cap.isOpened(): raise RuntimeError(f'Cannot open {path}')
        fps = cap.get(cv2.CAP_PROP_FPS); total = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
        begin = 0 if args.all_roads else round(args.start * fps)
        end = total if args.all_roads else min(total, begin + round(args.seconds * fps))
        if fps <= 0 or begin >= end: raise ValueError(f'Empty clip or invalid FPS: {path}')
        cap.set(cv2.CAP_PROP_POS_FRAMES, begin)
        destination = output / f'{path.stem}_ufld.mp4'
        writer = cv2.VideoWriter(str(destination), cv2.CAP_MSMF, cv2.VideoWriter_fourcc(*'H264'), fps, (1920, 540))
        if not writer.isOpened(): raise RuntimeError(f'Cannot create {destination}')
        filtering = LaneCurveFilter(fps)
        tracker = RoadTracker(fps=fps, classes=len(objects.names))
        rows, timing, pipeline, counts, written = [], [], [], Counter(), 0
        clock_start = time.perf_counter()
        print(f'START {path.name} frames {begin}:{end}', flush=True)
        try:
            for index in range(begin, end):
                ok, original = cap.read()
                if not ok: break
                t = time.perf_counter()
                raw = detector(original)
                timing.append(detector.last_ms)
                filtered = filtering.update(raw)
                raw_display = {k: dict(points=v, predicted=False) for k, v in raw.items()}
                # Object coordinates refer to the displayed 960x540 frame.
                frame = cv2.resize(original, (960, 540))
                result = objects.predict(frame, conf=.1, imgsz=640, device=0, verbose=False)[0]
                tracks = tracker.update(result.boxes.data.cpu().numpy()[:, :6])
                frame = draw_tracks(frame, tracks, objects.names)
                offset = lane_offset(filtered)
                raw_view = draw_curves(frame, raw_display, 'LEFT: UFLD v2 raw pretrained curves', lane_offset(raw_display))
                smooth_view = draw_curves(frame, filtered, 'RIGHT: curves + EMA (dashed = PRED)', offset)
                comparison = np.hstack([raw_view, smooth_view])
                elapsed = (time.perf_counter() - t) * 1000
                pipeline.append(elapsed)
                cv2.putText(comparison, f'VIDEO {index / fps:.2f}s | GPU lane {detector.last_ms:.1f}ms | geometry only, no accuracy labels',
                            (12, 525), cv2.FONT_HERSHEY_SIMPLEX, .6, (255, 255, 255), 1, cv2.LINE_AA)
                writer.write(comparison); written += 1
                counts[str(len(raw)) + '_raw_lanes'] += 1
                if offset is not None: counts['two_observed_ego_slots_geometry_valid'] += 1
                if written == (end - begin) // 2:
                    cv2.imwrite(str(output / f'{path.stem}_preview.jpg'), comparison)
                if offset is not None and not (output / f'{path.stem}_pair_example.jpg').exists():
                    cv2.imwrite(str(output / f'{path.stem}_pair_example.jpg'), comparison)
                rows.append(dict(frame=index, time_seconds=index / fps, raw_slots=list(raw),
                    held_slots=[k for k, v in filtered.items() if v['predicted']], offset_candidate=offset,
                    gpu_lane_ms=detector.last_ms))
                if args.show:
                    cv2.imshow('UFLD pretrained lane comparison', cv2.resize(comparison, (1536, 432)))
                    if cv2.waitKey(1) & 0xff == ord('q'): break
                if written % 120 == 0:
                    print(f'PROGRESS {written}/{end - begin}, lanes {list(raw)}, GPU {np.mean(timing[-120:]):.1f}ms', flush=True)
        finally:
            cap.release(); writer.release()
            if args.show: cv2.destroyAllWindows()
        (output / f'{path.stem}_events.json').write_text(json.dumps(rows, indent=2), encoding='utf-8')
        usable_timing = timing[5:] or timing
        item = dict(source=str(path), output=str(destination), fps=fps, start_frame=begin, frames=written,
                    counts=dict(counts), gpu_mean_ms_after_5_warmup=float(np.mean(usable_timing)),
                    gpu_p95_ms=float(np.percentile(usable_timing, 95)),
                    pipeline_mean_ms=float(np.mean(pipeline[5:] or pipeline)),
                    wall_seconds=time.perf_counter() - clock_start,
                    gpu_peak_allocated_GiB=torch.cuda.max_memory_allocated()/2**30)
        summary['videos'].append(item)
        (output / 'report.json').write_text(json.dumps(summary, indent=2), encoding='utf-8')
        print(json.dumps(item, indent=2), flush=True)
    print(f'COMPLETE {output}', flush=True)


if __name__ == '__main__': main()
