"""Compare raw detections (left) with CPU ByteTrack + smoothing (right)."""
from pathlib import Path
from project_paths import PROJECT_ROOT, DETECTOR_WEIGHTS
import argparse
import json
import time

import cv2
import numpy as np
from ultralytics import YOLO
from ultralytics.utils.files import increment_path
from road_tracker import RoadTracker


def draw_tracks(frame, tracks, names):
    image = frame.copy()
    for track in tracks:
        color = tuple(int(v) for v in ((np.array([73, 151, 211]) * (track['id'] + 1)) % 200 + 55))
        x1, y1, x2, y2 = np.rint(track['box']).astype(int)
        x1, x2 = np.clip([x1, x2], 0, image.shape[1] - 1)
        y1, y2 = np.clip([y1, y2], 0, image.shape[0] - 1)
        if x2 <= x1 or y2 <= y1:
            continue
        if track['predicted']:
            # Dashed boxes disclose a predicted position during brief misses.
            for x in range(x1, x2, 16):
                cv2.line(image, (x, y1), (min(x + 8, x2), y1), color, 2)
                cv2.line(image, (x, y2), (min(x + 8, x2), y2), color, 2)
            for y in range(y1, y2, 16):
                cv2.line(image, (x1, y), (x1, min(y + 8, y2)), color, 2)
                cv2.line(image, (x2, y), (x2, min(y + 8, y2)), color, 2)
        else:
            cv2.rectangle(image, (x1, y1), (x2, y2), color, 2)
        label = f"{names[track['cls']]} #{track['id']}" + (' PRED' if track['predicted'] else '')
        cv2.putText(image, label, (x1, max(16, y1 - 5)), cv2.FONT_HERSHEY_SIMPLEX, .6, color, 2)
    return image


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', type=Path, help='One video; otherwise process all real-road videos')
    args = parser.parse_args()
    root = PROJECT_ROOT
    output = increment_path(root / 'runs' / 'video_track_n640', mkdir=True)
    paths = [args.source.resolve()] if args.source else sorted(
        p for p in (root / 'video').glob('*.mp4') if p.name != 'video_20260929_111440.mp4')
    model = YOLO(str(DETECTOR_WEIGHTS))
    report = dict(detector_input_conf=.1, raw_display_conf=.25, alpha=.65,
                  tracker='class-separated ByteTrack', videos=[])
    for path in paths:
        cap = cv2.VideoCapture(str(path))
        fps = cap.get(cv2.CAP_PROP_FPS)
        total = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
        if not cap.isOpened() or fps <= 0:
            raise RuntimeError(f'Cannot open {path}')
        cap.release()
        tracker = RoadTracker(fps=fps, classes=len(model.names))
        video = output / f'{path.stem}_compare.mp4'
        writer = cv2.VideoWriter(str(video), cv2.CAP_MSMF, cv2.VideoWriter_fourcc(*'H264'), fps, (1920, 540))
        if not writer.isOpened():
            raise RuntimeError(f'Cannot create {video}')
        timing, rendered, predicted = [], 0, 0
        print(f'START {path.name} frames={total}', flush=True)
        try:
            for index, result in enumerate(model.predict(str(path), stream=True, conf=.1,
                    imgsz=640, iou=.7, device=0, verbose=False, save=False)):
                detections = result.boxes.data.cpu().numpy()[:, :6]
                begin = time.perf_counter()
                tracks = tracker.update(detections)
                timing.append((time.perf_counter() - begin) * 1000)
                raw = result[result.boxes.conf >= .25].plot(line_width=2, font_size=10)
                stabilized = draw_tracks(result.orig_img, tracks, result.names)
                pair = np.hstack([cv2.resize(raw, (960, 540)), cv2.resize(stabilized, (960, 540))])
                cv2.rectangle(pair, (0, 0), (1920, 28), (0, 0, 0), -1)
                cv2.putText(pair, 'LEFT: raw detections conf=0.25', (10, 20), cv2.FONT_HERSHEY_SIMPLEX, .6, (255, 255, 255), 1)
                cv2.putText(pair, 'RIGHT: ByteTrack + smoothing (dashed=PRED)', (970, 20), cv2.FONT_HERSHEY_SIMPLEX, .6, (255, 255, 255), 1)
                writer.write(pair)
                if index == total // 2:
                    cv2.imwrite(str(output / f'{path.stem}_compare.jpg'), pair)
                predicted += sum(track['predicted'] for track in tracks)
                rendered += 1
                if rendered % 1000 == 0:
                    print(f'PROGRESS {path.name} {rendered}/{total}', flush=True)
        finally:
            writer.release()
        assert rendered == total, (path.name, rendered, total)
        report['videos'].append(dict(source=str(path), output=str(video), frames=rendered,
            hold_frames=tracker.hold_frames, tracker_mean_ms=float(np.mean(timing)),
            tracker_p95_ms=float(np.percentile(timing, 95)), predicted_box_frames=predicted))
        (output / 'report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
        print(f'DONE {path.name} CPU tracking={np.mean(timing):.2f} ms/frame', flush=True)
    print(f'COMPLETE {output}', flush=True)


if __name__ == '__main__':
    main()
