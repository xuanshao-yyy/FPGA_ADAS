"""Four controlled variants, one runtime and scheduler. Never modifies weights."""
from pathlib import Path
import argparse
import datetime
import hashlib
import json
import os
import subprocess

ROOT = Path(__file__).resolve().parents[1]
VARIANTS = {'A': ('YOLOv8', False), 'B': ('YOLO11', False),
            'C': ('YOLOv8', True), 'D': ('YOLO11', True)}


def main():
    parser = argparse.ArgumentParser(description='A=v8+HSV B=11+HSV C=v8+RGB D=11+RGB. EXIT the current test before switching.')
    parser.add_argument('variant', choices=VARIANTS)
    parser.add_argument('scene', nargs='?', default='intersection')
    parser.add_argument('--mode', choices=['interactive', 'evidence', 'performance'], default='interactive')
    parser.add_argument('--output-root', type=Path, default=ROOT / 'player/results/comparison')
    parser.add_argument('--dry-run', action='store_true')
    args, extra = parser.parse_known_args()
    # Keep variant provenance true: these choices belong to this selector.
    reserved = {'--model', '--front-model', '--light-rgb-model'}
    if any(item.split('=', 1)[0] in reserved for item in extra):
        parser.error('Select detector/color through A/B/C/D; model override flags are not allowed.')
    detector, rgb = VARIANTS[args.variant]
    if detector == 'YOLOv8':
        candidates = ([Path(os.environ['V8_MODEL_DIR'])] if os.environ.get('V8_MODEL_DIR') else
                      [Path('/userdata/test_auto_drive/rk3568_scene_core_20261018/models')])
        model_dir = next((p for p in candidates if all((p / f).is_file() for f in
                         ['yolov8n_bdd8_i8.rknn', 'yolov8n_bdd8_640x384_i8.rknn'])), None)
        if model_dir is None:
            parser.error('Old YOLOv8 weights missing. Set V8_MODEL_DIR to the existing directory containing yolov8n_bdd8_i8.rknn and yolov8n_bdd8_640x384_i8.rknn.')
        square = model_dir / 'yolov8n_bdd8_i8.rknn'
        front = model_dir / 'yolov8n_bdd8_640x384_i8.rknn'
    else:
        square = ROOT / 'models/traffic_yolo11n_640x640_i8.rknn'
        front = ROOT / 'models/traffic_yolo11n_640x384_i8.rknn'
    color = ROOT / 'models/light_rgb_128_i8_kl256.rknn' if rgb else None
    paths = [square, front] + ([color] if color else [])
    for path in paths:
        if not path.is_file():
            parser.error('Model missing: ' + str(path))
    label = args.variant + ' ' + detector + ('+HSV_RGB_CONSENSUS' if rgb else '+HSV')
    stamp = datetime.datetime.now().strftime('%Y%m%d_%H%M%S_%f')
    output = args.output_root.resolve() / (args.variant + '_' + args.scene + '_' + stamp)
    command = ['bash', str(ROOT / 'run_test.sh'), args.scene, '--mode', args.mode,
               '--output-root', str(output), '--model', str(square), '--front-model', str(front),
               '--light-rgb-model', str(color) if color else 'none'] + extra
    meta = {'variant': args.variant, 'label': label, 'scene': args.scene, 'mode': args.mode,
            'runtime': str(ROOT / 'rk_signal_control'), 'command': command,
            'models': {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in paths},
            'note': 'Same core and default 6/3/1 scheduler. RGB requires HSV agreement. No accuracy acceptance implied.'}
    print(json.dumps(meta, indent=2), flush=True)
    if args.dry_run:
        return 0
    output.mkdir(parents=True, exist_ok=False)
    (output / 'variant.json').write_text(json.dumps(meta, indent=2) + '\n', encoding='utf-8')
    result = subprocess.run(command, env=os.environ | {'RK_MODEL_LABEL': label}, cwd=ROOT)
    (output / 'exit_code.txt').write_text(str(result.returncode) + '\n')
    return result.returncode if result.returncode >= 0 else 128 - result.returncode


if __name__ == '__main__':
    raise SystemExit(main())
