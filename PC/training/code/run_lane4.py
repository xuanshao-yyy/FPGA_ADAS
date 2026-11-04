"""Preview a locally fine-tuned four-class Small/Nano checkpoint on a road video."""
import argparse
import hashlib
import json
from pathlib import Path

import cv2
import numpy as np
import torch

from project_paths import PROJECT_ROOT
from twinlite_lane import prepare_rgb, prepare_tensor
from train_lane4 import create_model
from lane4_data import NAMES, overlay_labels


def load_checkpoint(path, device):
    saved = torch.load(path, map_location='cpu', weights_only=True)
    if (saved.get('classes') != NAMES or saved.get('variant') not in {'small', 'nano'}
            or saved.get('preprocessing') != 'RGB /255,640x384 letterbox114'):
        raise RuntimeError('Requires this project\'s four-class Small/Nano checkpoint, not a YOLO/binary PTH')
    model = create_model('cpu', saved['variant'])
    model.load_state_dict(saved['state_dict'], strict=True)
    return model.eval().to(device), saved


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--checkpoint', type=Path, default=PROJECT_ROOT/'runs/lane4_small_bdd-5/weights/best.pt')
    parser.add_argument('--source', type=Path, default=PROJECT_ROOT/'video/video_20260929_113539.mp4')
    parser.add_argument('--start', type=float, default=35)
    parser.add_argument('--seconds', type=float, default=12)
    parser.add_argument('--cpu', action='store_true')
    parser.add_argument('--show', action='store_true')
    args = parser.parse_args()
    if args.start < 0 or args.seconds <= 0:
        parser.error('start >= 0 and seconds > 0 required')
    if not args.checkpoint.is_file():
        parser.error(f'Checkpoint does not exist: {args.checkpoint}; train first or specify --checkpoint')
    torch.set_num_threads(4)
    cv2.setNumThreads(2)
    device = 'cuda:0' if torch.cuda.is_available() and not args.cpu else 'cpu'
    model, saved = load_checkpoint(args.checkpoint, device)
    cap = cv2.VideoCapture(str(args.source.resolve()))
    if not cap.isOpened():
        raise RuntimeError(f'Cannot open video: {args.source}')
    fps, total = cap.get(cv2.CAP_PROP_FPS), int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
    if fps <= 0:
        cap.release()
        raise RuntimeError('Invalid source FPS')
    begin = round(args.start * fps)
    end = min(total, begin + round(args.seconds * fps))
    if begin >= end:
        cap.release()
        raise RuntimeError('Empty requested clip')
    cap.set(cv2.CAP_PROP_POS_FRAMES, begin)
    output = PROJECT_ROOT/'runs/lane4_video'
    index = 1
    while output.exists():
        index += 1
        output = PROJECT_ROOT/f'runs/lane4_video-{index}'
    output.mkdir()
    destination = output/'comparison.mp4'
    writer = cv2.VideoWriter(str(destination), cv2.CAP_MSMF, cv2.VideoWriter_fourcc(*'H264'), fps, (1280, 360))
    if not writer.isOpened():
        cap.release()
        raise RuntimeError('Cannot create H264 comparison video')
    frames = 0
    smoke = saved.get('smoke', False)
    with torch.inference_mode():
        try:
            for _ in range(begin, end):
                ok, image = cap.read()
                if not ok:
                    break
                view = cv2.resize(image, (640, 360))
                rgb, (left, top, width, height) = prepare_rgb(view)
                logits = model(prepare_tensor(rgb).to(device)).cpu().numpy()
                if logits.shape != (1, 4, 384, 640) or not np.isfinite(logits).all():
                    raise RuntimeError('Invalid four-class model output')
                # Restore each channel before argmax; remove the letterbox padding.
                restored = np.stack([cv2.resize(channel[top:top+height, left:left+width], (640, 360))
                                     for channel in logits[0]])
                prediction = restored.argmax(0).astype(np.uint8)
                display = overlay_labels(view, prediction)
                original = view.copy()
                for panel, title in [(original, 'Original road image'),
                                     (display, 'SMOKE CHECK ONLY' if smoke else 'Lane4 semantic candidates')]:
                    cv2.putText(panel, title, (8, 25), cv2.FONT_HERSHEY_SIMPLEX, .6, (255, 255, 255), 2)
                cv2.putText(display, 'Green solid | Blue dashed | Magenta crosswalk', (8, 344),
                            cv2.FONT_HERSHEY_SIMPLEX, .43, (255, 255, 255), 1)
                comparison = np.hstack([original, display])
                writer.write(comparison)
                frames += 1
                if frames == max(1, (end-begin)//2):
                    cv2.imwrite(str(output/'preview.jpg'), comparison)
                if frames % 120 == 0:
                    print('PROGRESS', frames, '/', end-begin, flush=True)
                if args.show:
                    cv2.imshow('Lane4 preview', comparison)
                    if cv2.waitKey(1) & 0xff == ord('q'):
                        break
        finally:
            writer.release()
            cap.release()
            if args.show:
                cv2.destroyAllWindows()
    verify = cv2.VideoCapture(str(destination))
    decoded, _ = verify.read()
    saved_frames = int(verify.get(cv2.CAP_PROP_FRAME_COUNT))
    verify.release()
    if not frames or not decoded or saved_frames != frames:
        raise RuntimeError('Saved video frame/decode verification failed')
    report = dict(variant=saved['variant'], checkpoint=str(args.checkpoint.resolve()),
                  checkpoint_sha256=hashlib.sha256(args.checkpoint.read_bytes()).hexdigest(),
                  training_epoch=saved['epoch'], smoke=smoke, classes=NAMES,
                  source=str(args.source.resolve()), start_frame=begin, frames=frames, device=device,
                  board_tested=False, ground_truth_available=False, ego_lane_selected=False,
                  decision_logic_available=False, model_finalized=False,
                  output=str(destination), saved_video_verified=True)
    (output/'report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print('COMPLETE', output, flush=True)


if __name__ == '__main__':
    main()
