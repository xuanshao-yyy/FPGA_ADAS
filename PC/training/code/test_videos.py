"""Run the trained detector on local videos without changing the inputs."""
from collections import Counter
from pathlib import Path
from project_paths import PROJECT_ROOT, DETECTOR_WEIGHTS
import argparse
import hashlib
import json
import time

import cv2
import numpy as np
from ultralytics import YOLO
from ultralytics.utils.files import increment_path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--model', type=Path, default=DETECTOR_WEIGHTS, help='Detection checkpoint; default keeps the original model')
    parser.add_argument('--source', type=Path, nargs='+', help='One or more input video files; default uses the road video folder')
    parser.add_argument('--output', type=Path, default=PROJECT_ROOT / 'runs/video_test_n640', help='Output folder; existing folders receive a numeric suffix')
    args = parser.parse_args()
    if not args.model.is_file():
        parser.error(f'Model does not exist: {args.model}')
    root = PROJECT_ROOT
    output = increment_path(args.output, mkdir=True)
    samples = output / "samples"
    samples.mkdir()
    # The user only evaluates real roads, excluding the 3D dashboard display.
    videos = sorted(args.source) if args.source else sorted(p for p in (root / "video").glob("*.mp4")
                    if p.name != "video_20260929_111440.mp4")
    if not videos or any(not p.is_file() for p in videos):
        parser.error('Provide existing video files')
    if len({p.stem for p in videos}) != len(videos):
        parser.error('Input videos must have different stems to keep output names distinct')
    metadata = []
    raw_tiles = []
    for path in videos:
        cap = cv2.VideoCapture(str(path))
        if not cap.isOpened():
            raise RuntimeError(f"Cannot open {path}")
        count = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
        fps = cap.get(cv2.CAP_PROP_FPS)
        width = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
        height = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
        indices = sorted({0, count // 4, count // 2, 3 * count // 4, count - 1})
        for index in indices:
            cap.set(cv2.CAP_PROP_POS_FRAMES, index)
            ok, frame = cap.read()
            if not ok:
                raise RuntimeError(f"Cannot decode {path.name} frame {index}")
            cv2.imwrite(str(samples / f"{path.stem}_{index:05d}_raw.jpg"), frame)
            if index == count // 2:
                tile = cv2.resize(frame, (640, 360))
                tile = cv2.copyMakeBorder(tile, 28, 0, 0, 0, cv2.BORDER_CONSTANT)
                cv2.putText(tile, path.name, (8, 20), cv2.FONT_HERSHEY_SIMPLEX, .55, (255, 255, 255), 1)
                raw_tiles.append(tile)
        cap.release()
        metadata.append(dict(file=path.name, frames=count, fps=fps, width=width,
                             height=height, sample_indices=indices))
    while len(raw_tiles) % 2:
        raw_tiles.append(np.zeros_like(raw_tiles[0]))
    sheet = np.vstack([np.hstack(raw_tiles[i:i + 2]) for i in range(0, len(raw_tiles), 2)])
    cv2.imwrite(str(output / "input_overview.jpg"), sheet)
    print(f"PREVIEW_READY {output}", flush=True)

    model_path = args.model.resolve()
    checkpoint_sha = hashlib.sha256(model_path.read_bytes()).hexdigest()
    model = YOLO(str(model_path))
    report = dict(model=str(model_path), model_sha256=checkpoint_sha, classes=model.names,
                  imgsz=640, conf=.25, iou=.7, tracking=False,
                  note="Frame detection counts are not unique objects or accuracy metrics.", videos=[])
    for path, info in zip(videos, metadata):
        destination = output / f"{path.stem}_pred.mp4"
        writer = cv2.VideoWriter(str(destination), cv2.CAP_MSMF, cv2.VideoWriter_fourcc(*"H264"),
                                 info["fps"], (info["width"], info["height"]))
        if not writer.isOpened():
            raise RuntimeError(f"Cannot create {destination}")
        counts = Counter()
        speed_totals = Counter()
        processed = 0
        start = time.perf_counter()
        print(f"START {path.name} frames={info['frames']}", flush=True)
        try:
            for index, result in enumerate(model.predict(source=str(path), stream=True,
                    imgsz=640, conf=.25, iou=.7, device=0, save=False, verbose=False)):
                annotated = result.plot(line_width=2, font_size=10)
                writer.write(annotated)
                counts.update(result.names[int(c)] for c in result.boxes.cls.tolist())
                speed_totals.update(result.speed)
                if index in info["sample_indices"]:
                    cv2.imwrite(str(samples / f"{path.stem}_{index:05d}_pred640.jpg"), annotated)
                processed += 1
                if processed % 1000 == 0:
                    print(f"PROGRESS {path.name} {processed}/{info['frames']}", flush=True)
        finally:
            writer.release()
        if processed != info["frames"]:
            raise RuntimeError(f"Decoded {processed}/{info['frames']} frames: {path.name}")
        info.update(output=str(destination), processed_frames=processed,
                    seconds_to_process=round(time.perf_counter() - start, 2),
                    mean_model_speed_ms={key: value / processed for key, value in speed_totals.items()},
                    frame_detection_counts=dict(counts))
        report["videos"].append(info)
        (output / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
        print(f"DONE {path.name} {processed} frames", flush=True)
    if hashlib.sha256(model_path.read_bytes()).hexdigest() != checkpoint_sha:
        raise RuntimeError('Model checksum changed during inference')
    report['model_unchanged'] = True
    (output / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(f"COMPLETE {output}", flush=True)


if __name__ == "__main__":
    main()
