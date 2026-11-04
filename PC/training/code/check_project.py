"""Validate project locations without training or processing full videos."""
import json
import hashlib
from pathlib import Path

import yaml

from project_paths import PROJECT_ROOT, DATA_ARCHIVES, DETECTOR_WEIGHTS


def main():
    required = [DETECTOR_WEIGHTS, PROJECT_ROOT / 'weights/pretrained/yolov8n.pt',
                PROJECT_ROOT / 'weights/ufld_v2/culane_res18.pth',
                PROJECT_ROOT / 'weights/yolop/yolop-640-640.onnx', PROJECT_ROOT / 'assets/bus.jpg']
    required += [DATA_ARCHIVES / name for name in ('bdd100k_images_100k.zip', 'bdd100k_labels.zip',
                 'bdd100k_det_20_labels.zip', 'dataset_train_rgb.zip', 'bstld-master.zip', 'Cone-Detection-main.zip')]
    for path in required:
        if not path.is_file() or path.stat().st_size == 0:
            raise ValueError(f'Missing/empty required file: {path}')
    configs = [PROJECT_ROOT / 'datasets' / name for name in ('bdd8/bdd8.yaml', 'bdd8/bdd8_smoke.yaml',
               'bdd8/bdd8_starter.yaml', 'bosch_light/bosch_light.yaml', 'tracon_cone/tracon_cone.yaml')]
    summaries = []
    for path in configs:
        config = yaml.safe_load(path.read_text(encoding='utf-8'))
        base = Path(config['path'])
        if not base.is_dir():
            raise ValueError(f'Dataset base missing: {path} -> {base}')
        for split in ('train', 'val', 'test'):
            target = base / config[split]
            if not target.exists():
                raise ValueError(f'Dataset split missing: {target}')
            if target.is_file():
                images = [Path(line) for line in target.read_text(encoding='utf-8').splitlines() if line]
                missing = [str(image) for image in images if not image.is_file() or
                           not (base / 'labels' / split / (image.stem + '.txt')).is_file()]
                if missing:
                    raise ValueError(f'Broken subset list {target}: {missing[:3]}')
            else:
                first = next((p for p in target.iterdir() if p.suffix.lower() in ('.jpg', '.jpeg', '.png', '.bmp')), None)
                if first is None or not (base / 'labels' / split / (first.stem + '.txt')).is_file():
                    raise ValueError(f'No matching image/label sample: {target}')
        summaries.append(dict(config=str(path), classes=len(config['names'])))
    videos = list((PROJECT_ROOT / 'video').glob('*.mp4'))
    for suffix in ('111440', '112454', '112507', '112739', '113343', '113421', '113452', '113539'):
        path = PROJECT_ROOT / 'video' / f'video_20260929_{suffix}.mp4'
        if not path.is_file():
            raise ValueError(f'Original video missing: {path}')
    if not (PROJECT_ROOT / 'yolov8n.pt').samefile(PROJECT_ROOT / 'weights/pretrained/yolov8n.pt'):
        raise ValueError('Legacy YOLO weight link differs from managed weights')
    # Protected models and training configuration/list files must remain unchanged.
    manifest = json.loads((PROJECT_ROOT / 'maintenance/layout_20261002/migration.json').read_text(encoding='utf-8'))
    for name, expected in manifest['protected'].items():
        path = PROJECT_ROOT / name
        value = hashlib.sha256()
        with path.open('rb') as stream:
            for chunk in iter(lambda: stream.read(8 * 1024 * 1024), b''):
                value.update(chunk)
        if value.hexdigest() != expected:
            raise ValueError(f'Protected file changed: {path}')
    report = dict(status='PASS', dataset_configs=summaries, preserved_original_videos=8, available_videos=len(videos),
                  archives=len(list(DATA_ARCHIVES.glob('*.zip'))), protected_files=len(manifest['protected']),
                  detector_weights=str(DETECTOR_WEIGHTS))
    destination = PROJECT_ROOT / 'maintenance/layout_20261002/path_verification.json'
    destination.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
