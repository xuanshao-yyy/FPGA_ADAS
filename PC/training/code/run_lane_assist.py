"""Compare CPU Hough with YOLOP segmentation + Hough on real-road video.

PC demo: trained YOLOv8/ByteTrack objects run on GPU; lane ONNX runs on CPU.
No model weights or hardware project are modified. Output uses source duration
and frame rate; evidence refresh rate is separately disclosed on screen.
"""
import argparse
from collections import Counter
import json
from pathlib import Path
from project_paths import PROJECT_ROOT, DETECTOR_WEIGHTS, YOLOP_WEIGHTS
import time

import cv2
import numpy as np
from ultralytics import YOLO
from ultralytics.utils.files import increment_path

from lane_assist import LaneMasks, LaneAssist, extract_boundaries, draw_lanes
from road_tracker import RoadTracker
from track_videos import draw_tracks


def main():
    root = PROJECT_ROOT
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', type=Path, default=root / 'video/video_20260929_113539.mp4')
    parser.add_argument('--start', type=float, default=0)
    parser.add_argument('--seconds', type=float, help='Default: whole video')
    parser.add_argument('--stride', type=int, default=3, help='30fps / 3 = 10 perception updates per video second')
    parser.add_argument('--ego-x', type=float, default=.5, help='Image x/w of ego axis; needs camera calibration')
    parser.add_argument('--command', choices=['straight', 'left', 'right'], default='straight')
    parser.add_argument('--force', action='store_true', help='Simulate forcing a lane change request')
    parser.add_argument('--show', action='store_true', help='Open local comparison window; A/D/S command, F force, Q exit')
    parser.add_argument('--hough-only', action='store_true', help='Skip lane neural model; compare original vs Hough')
    args = parser.parse_args()
    if args.stride < 1 or args.start < 0 or (args.seconds is not None and args.seconds <= 0):
        parser.error('stride >= 1, start >= 0, seconds > 0 required')
    cv2.setNumThreads(4)
    cap = cv2.VideoCapture(str(args.source))
    if not cap.isOpened(): raise RuntimeError(f'Cannot open {args.source}')
    fps = cap.get(cv2.CAP_PROP_FPS)
    total = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
    begin = round(args.start * fps)
    end = min(total, begin + round(args.seconds * fps)) if args.seconds else total
    if fps <= 0 or begin >= end: raise ValueError('Invalid FPS or empty selected segment')
    cap.set(cv2.CAP_PROP_POS_FRAMES, begin)
    output = increment_path(root / 'runs/lane_assist', mkdir=True)
    destination = output / f'{args.source.stem}_lanes.mp4'
    writer = cv2.VideoWriter(str(destination), cv2.CAP_MSMF, cv2.VideoWriter_fourcc(*'H264'), fps, (1920, 540))
    if not writer.isOpened(): raise RuntimeError('Cannot create H264 output')
    model = YOLO(str(DETECTOR_WEIGHTS))
    tracker = RoadTracker(fps=fps / args.stride, classes=len(model.names))
    backends = [LaneMasks()]
    if not args.hough_only:
        backends.append(LaneMasks(YOLOP_WEIGHTS))
    states = [LaneAssist(args.ego_x, fps / args.stride) for _ in backends]
    counts = [Counter() for _ in backends]
    timing = [[] for _ in backends]
    rows, result, written, updates = [], None, 0, 0
    command, force = args.command, args.force
    start_clock = time.perf_counter()
    print(f'START {args.source.name}: frames {begin}:{end}, perception {fps / args.stride:.1f}Hz', flush=True)
    try:
        for index in range(begin, end):
            ok, original = cap.read()
            if not ok: break
            frame = cv2.resize(original, (960, 540))
            if (index - begin) % args.stride == 0:
                detection = model.predict(frame, imgsz=640, conf=.1, device=0, verbose=False)[0]
                tracks = tracker.update(detection.boxes.data.cpu().numpy()[:, :6])
                objects = draw_tracks(frame, tracks, model.names)
                blocked = np.zeros(frame.shape[:2], np.uint8)
                for box in detection.boxes.data.cpu().numpy():
                    if box[5] <= 6 and box[4] >= .25:
                        x1, y1, x2, y2 = np.rint(box[:4]).astype(int)
                        cv2.rectangle(blocked, (max(0, x1 - 3), max(0, y1 - 3)),
                                      (min(959, x2 + 3), min(539, y2 + 3)), 255, -1)
                statuses, observations = [], []
                for n, (backend, state) in enumerate(zip(backends, states)):
                    t = time.perf_counter()
                    mask, paint = backend(frame)
                    boundaries = extract_boundaries(mask, paint, blocked)
                    status = state.update(boundaries, command, force)
                    timing[n].append((time.perf_counter() - t) * 1000)
                    counts[n][status['state']] += 1
                    mask[:round(.55 * 540)] = 0
                    mask[round(.94 * 540):] = 0
                    mask[blocked > 0] = 0
                    observations.append((mask, status))
                    statuses.append({k: v for k, v in status.items() if k != 'pair'})
                rows.append(dict(frame=index, time_seconds=index / fps, command=command, force=force,
                                 hough=statuses[0], yolop=None if args.hough_only else statuses[1]))
                updates += 1
            # Keep current background frames; reuse only last perception evidence.
            objects = draw_tracks(frame, tracks, model.names)
            views = []
            for n, (mask, status) in enumerate(observations):
                title = ('HOUGH' if n == 0 else 'YOLOP + HOUGH') + f' | sampling {fps / args.stride:.1f}Hz OFFLINE | prototype'
                views.append(draw_lanes(objects, mask, status, title))
            display = np.hstack([frame, views[0]]) if args.hough_only else np.hstack(views)
            age = ((index - begin) % args.stride) / fps * 1000
            cv2.putText(display, f'VIDEO {index / fps:.2f}s | evidence age {age:.0f}ms | NO METRIC CALIBRATION',
                        (12, 525), cv2.FONT_HERSHEY_SIMPLEX, .6, (255, 255, 255), 1, cv2.LINE_AA)
            writer.write(display)
            written += 1
            if written == (end - begin) // 2: cv2.imwrite(str(output / 'preview.jpg'), display)
            if args.show:
                cv2.imshow('Lane assist prototype', cv2.resize(display, (1536, 432)))
                key = cv2.waitKey(1) & 0xff
                if key == ord('q'): break
                if key in (ord('a'), ord('d'), ord('s')):
                    command = {ord('a'): 'left', ord('d'): 'right', ord('s'): 'straight'}[key]
                if key == ord('f'): force = not force
            if updates % 50 == 0 and (index - begin) % args.stride == 0:
                print(f'PROGRESS {written}/{end - begin}, states {[r["state"] for r in statuses]}', flush=True)
    finally:
        writer.release(); cap.release()
        if args.show: cv2.destroyAllWindows()
    (output / 'events.json').write_text(json.dumps(rows, indent=2), encoding='utf-8')
    summary = dict(source=str(args.source.resolve()), output=str(destination), start_frame=begin,
                   written_frames=written, updates=updates, fps=fps, perception_hz=fps / args.stride,
                   wall_seconds=time.perf_counter() - start_clock, ego_axis=args.ego_x,
                   camera_calibrated=False, ground_truth_available=False, backends=[])
    for n in range(len(backends)):
        summary['backends'].append(dict(name='hough' if n == 0 else 'yolop_hough',
            state_counts=dict(counts[n]), cpu_mean_ms=float(np.mean(timing[n])),
            cpu_p95_ms=float(np.percentile(timing[n], 95))))
    (output / 'report.json').write_text(json.dumps(summary, indent=2), encoding='utf-8')
    print(json.dumps(summary, indent=2), flush=True)


if __name__ == '__main__': main()
