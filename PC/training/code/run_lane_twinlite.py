"""Compare Small/Nano official lane candidates on a short video; no training or driving decisions."""
import argparse
import json
from pathlib import Path
import time

import cv2
import numpy as np
import torch

from project_paths import PROJECT_ROOT
from twinlite_lane import load_lane_model, prepare_rgb, prepare_tensor, restore_mask, overlay


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=PROJECT_ROOT / 'video/video_20260929_113539.mp4')
    parser.add_argument('--start', type=float, default=35)
    parser.add_argument('--seconds', type=float, default=12)
    parser.add_argument('--model', choices=['small', 'nano', 'both'], default='both')
    parser.add_argument('--cpu', action='store_true')
    parser.add_argument('--show', action='store_true')
    args = parser.parse_args()
    if args.start < 0 or args.seconds <= 0:
        parser.error('start >= 0 and seconds > 0 required')
    torch.set_num_threads(4)
    cv2.setNumThreads(4)
    device = 'cuda:0' if torch.cuda.is_available() and not args.cpu else 'cpu'
    variants = ['small', 'nano'] if args.model == 'both' else [args.model]
    models = {name: load_lane_model(name, device) for name in variants}
    cap = cv2.VideoCapture(str(args.source.resolve()))
    if not cap.isOpened():
        raise RuntimeError(f'Cannot open video: {args.source}')
    fps = cap.get(cv2.CAP_PROP_FPS)
    total = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
    if fps <= 0:
        raise RuntimeError('Invalid source FPS')
    begin = round(args.start * fps)
    end = min(total, begin + round(args.seconds * fps))
    if begin >= end:
        raise RuntimeError('Empty requested clip')
    cap.set(cv2.CAP_PROP_POS_FRAMES, begin)
    output = PROJECT_ROOT / 'runs/lane_twinlite'
    index = 1
    while output.exists():
        index += 1
        output = PROJECT_ROOT / f'runs/lane_twinlite-{index}'
    output.mkdir()
    destination = output / f'{args.source.stem}_small_nano.mp4'
    writer = cv2.VideoWriter(str(destination), cv2.CAP_MSMF, cv2.VideoWriter_fourcc(*'H264'),
                             fps, (640 * (1 + len(variants)), 360))
    if not writer.isOpened():
        cap.release()
        raise RuntimeError(f'Cannot create H264 comparison: {destination}')
    times = {name: [] for name in variants}
    report = dict(source=str(args.source.resolve()), device=device, input_shape=[1, 3, 384, 640],
                  pretrained=True, trained_locally=False, board_tested=False,
                  lane_style_available=False, ego_lane_selection_available=False,
                  ground_truth_available=False, output=str(destination), models={}, frames=0)
    started = time.perf_counter()
    with torch.inference_mode():
        zero = torch.zeros(1, 3, 384, 640, device=device)
        for model in models.values():
            for _ in range(5):
                model(zero)
        try:
            for frame_number in range(begin, end):
                ok, original = cap.read()
                if not ok:
                    break
                view = cv2.resize(original, (640, 360))
                first = view.copy()
                cv2.putText(first, 'Original road image', (8, 24), cv2.FONT_HERSHEY_SIMPLEX,
                            .65, (255, 255, 255), 2, cv2.LINE_AA)
                panels = [first]
                rgb, content = prepare_rgb(view)
                tensor = prepare_tensor(rgb).to(device)
                for name, model in models.items():
                    if device.startswith('cuda'):
                        torch.cuda.synchronize()
                    tick = time.perf_counter()
                    logits = model(tensor)
                    if device.startswith('cuda'):
                        torch.cuda.synchronize()
                    times[name].append((time.perf_counter() - tick) * 1000)
                    mask = restore_mask(logits.cpu().numpy(), view.shape, content)
                    display = overlay(view, mask)
                    cv2.putText(display, f'{name} | lane candidates', (8, 24), cv2.FONT_HERSHEY_SIMPLEX,
                                .65, (255, 255, 255), 2, cv2.LINE_AA)
                    cv2.putText(display, 'No solid/dashed or ego-lane decision', (8, 347),
                                cv2.FONT_HERSHEY_SIMPLEX, .43, (255, 255, 255), 1, cv2.LINE_AA)
                    panels.append(display)
                comparison = np.hstack(panels)
                writer.write(comparison)
                report['frames'] += 1
                if report['frames'] == max(1, (end - begin) // 2):
                    cv2.imwrite(str(output / 'preview.jpg'), comparison)
                if report['frames'] % 120 == 0:
                    print('PROGRESS', report['frames'], '/', end - begin, flush=True)
                if args.show:
                    cv2.imshow('TwinLiteNet+ official lane candidates', comparison)
                    if cv2.waitKey(1) & 0xff == ord('q'):
                        break
        finally:
            writer.release()
            cap.release()
            if args.show:
                cv2.destroyAllWindows()
    if report['frames'] == 0:
        raise RuntimeError('No video frames processed')
    for name, model in models.items():
        report['models'][name] = dict(full_parameters=model.full_model_parameters,
                                     lane_only_parameters=sum(p.numel() for p in model.parameters()),
                                     pc_forward_mean_ms=float(np.mean(times[name])),
                                     pc_forward_p95_ms=float(np.percentile(times[name], 95)))
    report['wall_seconds'] = time.perf_counter() - started
    report['timing_scope'] = 'PC model forward only, synchronized CUDA; excludes resize/transfer/display; not RK3568 FPS'
    (output / 'report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    verify = cv2.VideoCapture(str(destination))
    saved_frames = int(verify.get(cv2.CAP_PROP_FRAME_COUNT))
    ok, _ = verify.read()
    verify.release()
    if not ok or saved_frames != report['frames']:
        raise RuntimeError(f'Video verification failed: {saved_frames} != {report["frames"]}')
    print('COMPLETE', output, flush=True)


if __name__ == '__main__':
    main()
