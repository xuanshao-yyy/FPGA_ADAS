"""Replay a local video with the eight-class detector and trained three-color ROI model."""
import argparse
from datetime import datetime
import json
from pathlib import Path
import time

import cv2
import numpy as np
import torch

from project_paths import PROJECT_ROOT as ROOT, DETECTOR_WEIGHTS
from light_color import crop_bounds, tensor_rgb
from light_rgb import NAMES, UNKNOWN, load_rgb_model, reject_uncertain


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True, help='Local video, not a network URL')
    parser.add_argument('--color', type=Path, required=True, help='Trained color/weights/best.pt, not YOLO PT')
    parser.add_argument('--detector', type=Path, default=DETECTOR_WEIGHTS)
    parser.add_argument('--device', choices=['cpu', '0'], default='0')
    parser.add_argument('--show', action='store_true')
    parser.add_argument('--conf', type=float, default=.15)
    parser.add_argument('--threshold', type=float, default=None)
    parser.add_argument('--margin', type=float, default=None)
    args = parser.parse_args()
    if not args.source.is_file() or not args.color.is_file() or not args.detector.is_file():
        parser.error('Source and both model files must exist locally')
    device = 'cuda:0' if args.device == '0' else 'cpu'
    if device != 'cpu' and not torch.cuda.is_available():
        parser.error('CUDA unavailable; use --device cpu')
    model, checkpoint = load_rgb_model(args.color, device)
    threshold = checkpoint['rejection']['threshold'] if args.threshold is None else args.threshold
    margin = checkpoint['rejection']['margin'] if args.margin is None else args.margin
    if not 0 <= args.conf <= 1 or not 0 <= threshold <= 1 or not 0 <= margin <= 1:
        parser.error('Confidence, threshold and margin must lie in [0,1]')
    from ultralytics import YOLO
    detector = YOLO(str(args.detector))
    if len(detector.names) != 8 or detector.names.get(7) != 'traffic_light':
        parser.error('Detector must have the original eight classes with traffic_light ID7')
    directory = ROOT/'runs'/('light_rgb_video_'+datetime.now().strftime('%Y%m%d_%H%M%S_%f'))
    directory.mkdir()
    capture = cv2.VideoCapture(str(args.source))
    if not capture.isOpened():
        raise ValueError('Cannot open video')
    fps = capture.get(cv2.CAP_PROP_FPS)
    if not np.isfinite(fps) or fps <= 0:
        fps = 30.
    writer = None
    frames, started = 0, time.perf_counter()
    palette = [(0, 0, 255), (0, 220, 255), (0, 220, 0), (170, 170, 170)]
    try:
        if args.show:
            cv2.namedWindow('Traffic-light RGB experiment: Q exits', cv2.WINDOW_NORMAL)
            cv2.resizeWindow('Traffic-light RGB experiment: Q exits', 1280, 720)
        while True:
            ok, frame = capture.read()
            if not ok:
                break
            h, w = frame.shape[:2]
            if writer is None:
                writer = cv2.VideoWriter(str(directory/'prediction.mp4'), cv2.VideoWriter_fourcc(*'mp4v'), fps, (w, h))
                if not writer.isOpened():
                    raise RuntimeError('Cannot create output MP4')
            prediction = detector.predict(frame, imgsz=640, conf=args.conf, device=args.device,
                                          classes=[7], verbose=False)[0]
            rgb = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
            boxes = prediction.boxes.xyxy.cpu().numpy()
            patches, positions = [], []
            labels, scores = [UNKNOWN]*len(boxes), [0.]*len(boxes)
            for i, box in enumerate(boxes):
                if min(box[2]-box[0], box[3]-box[1]) < 8:
                    continue
                x1, y1, x2, y2 = crop_bounds(box, w, h)
                patches.append(tensor_rgb(rgb[y1:y2, x1:x2]))
                positions.append(i)
            if patches:
                with torch.inference_mode():
                    probabilities = model(torch.stack(patches).to(device)).softmax(1).cpu()
                    accepted = reject_uncertain(probabilities, threshold, margin)
                for n, index in enumerate(positions):
                    labels[index], scores[index] = int(accepted[n]), float(probabilities[n].max())
            for box, label, score in zip(boxes, labels, scores):
                x1, y1, x2, y2 = np.round(box).astype(int)
                color = palette[label]
                text = f'{(NAMES+["UNKNOWN"])[label]} {score:.2f}'
                cv2.rectangle(frame, (x1, y1), (x2, y2), color, 2)
                cv2.putText(frame, text, (max(0, x1), max(20, y1-5)), cv2.FONT_HERSHEY_SIMPLEX, .65, color, 2)
            cv2.putText(frame, 'Experimental colors; UNKNOWN is not permission to proceed', (12, h-15),
                        cv2.FONT_HERSHEY_SIMPLEX, .6, (255, 255, 255), 1)
            writer.write(frame)
            frames += 1
            if args.show:
                cv2.imshow('Traffic-light RGB experiment: Q exits', frame)
                if cv2.waitKey(1) & 255 in (ord('q'), 27):
                    break
    finally:
        capture.release()
        if writer:
            writer.release()
        if args.show:
            cv2.destroyAllWindows()
    if not frames:
        raise ValueError('No video frames decoded')
    report = {'frames': frames, 'wall_seconds': time.perf_counter()-started,
              'source': str(args.source), 'detector': str(args.detector), 'color': str(args.color),
              'threshold': threshold, 'margin': margin,
              'scope': 'PC development preview, not measured accuracy or RK3568 performance'}
    (directory/'report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(f'Saved {directory / "prediction.mp4"}')


if __name__ == '__main__':
    main()
