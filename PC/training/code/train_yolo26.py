"""YOLO26n BDD8 experiments: check -> smoke -> starter/full, independent of YOLOv8."""
import argparse
import hashlib
import json
from pathlib import Path

from project_paths import PROJECT_ROOT

NAMES = ['person', 'rider', 'car', 'bus', 'truck', 'bicycle', 'motorcycle', 'traffic_light']
STAGES = {
    'smoke': ('bdd8_smoke.yaml', 1, 'bdd8_yolo26n_smoke'),
    'starter': ('bdd8_starter.yaml', 30, 'bdd8_yolo26n_starter'),
    'full': ('bdd8.yaml', 100, 'bdd8_yolo26n_full_n640'),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stage', choices=['check', *STAGES], default='check')
    parser.add_argument('--batch', type=int, default=8)
    parser.add_argument('--workers', type=int, default=None)
    args = parser.parse_args()
    if args.batch < 1 or (args.workers is not None and args.workers < 0):
        parser.error('batch must be positive and workers must be nonnegative')

    import torch
    import ultralytics
    import yaml
    from ultralytics import YOLO

    weight = PROJECT_ROOT / 'weights' / 'yolo26n.pt'
    if not weight.is_file():
        raise SystemExit(f'Missing local YOLO26n weight: {weight}')
    if not torch.cuda.is_available():
        raise SystemExit('CUDA unavailable. Use the existing adas_yolo environment; GPU training is required here.')
    stage = 'full' if args.stage == 'check' else args.stage
    data_name, epochs, experiment = STAGES[stage]
    data_path = PROJECT_ROOT / 'datasets' / 'bdd8' / data_name
    dataset = yaml.safe_load(data_path.read_text(encoding='utf-8'))
    names = dataset['names']
    if isinstance(names, dict):
        names = [names[k] for k in sorted(names)]
    if names != NAMES:
        raise SystemExit(f'Dataset class mapping differs from BDD8: {names}')
    dataset_root = Path(dataset['path'])
    for split in ('train', 'val'):
        split_path = dataset_root / dataset[split]
        if not split_path.exists():
            raise SystemExit(f'Missing {split} input: {split_path}')

    model = YOLO(str(weight))
    if model.task != 'detect' or model.model.model[-1].reg_max != 1:
        raise SystemExit('The selected checkpoint is not the expected DFL-free YOLO26 detection model')
    # cls_remap in this installed package matches exact names. Keep dataset IDs,
    # but alias COCO's space-separated light name in memory for weight transfer.
    source_names = dict(model.model.names)
    model.model.names = {k: ('traffic_light' if v == 'traffic light' else v) for k, v in source_names.items()}
    options = dict(
        data=str(data_path), epochs=epochs, imgsz=640, batch=args.batch, device=0,
        workers=args.workers if args.workers is not None else (0 if stage == 'smoke' else 2),
        optimizer='AdamW', lr0=.001, lrf=.01, momentum=.9, weight_decay=.0005,
        warmup_epochs=3., warmup_bias_lr=0., cos_lr=False,
        patience=20, close_mosaic=0 if stage == 'smoke' else 10,
        cache=False, amp=True, seed=42, deterministic=True, val=True,
        project=str(PROJECT_ROOT / 'runs'), name=experiment, exist_ok=False,
    )
    report = dict(
        stage=args.stage, ultralytics=ultralytics.__version__, torch=torch.__version__,
        gpu=torch.cuda.get_device_name(0), weight=str(weight),
        weight_sha256=hashlib.sha256(weight.read_bytes()).hexdigest(),
        classes=NAMES, model_parameters=sum(p.numel() for p in model.model.parameters()),
        pretrained_name_aliases={'traffic light': 'traffic_light'},
        training_options=options,
        scope='BDD8 architecture comparison; not traffic-light data strengthening or lane training',
    )
    print(json.dumps(report, ensure_ascii=False, indent=2), flush=True)
    if args.stage == 'check':
        folder = PROJECT_ROOT / 'maintenance' / 'yolo26_training_20261003'
        folder.mkdir(parents=True, exist_ok=True)
        (folder / 'preflight.json').write_text(json.dumps(report, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')
        print('CHECK complete. No training started.', flush=True)
        return
    model.train(**options)
    print(f'Actual results: {model.trainer.save_dir}', flush=True)
    print('Smoke checks validate execution only; compare trained models on the same full validation data and input mode.', flush=True)


if __name__ == '__main__':
    main()  # required for Windows multiprocessing DataLoader
