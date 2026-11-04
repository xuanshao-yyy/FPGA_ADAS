"""Export existing nuScenes real-road raw images to the same four-view replay interface."""
import argparse
import json
from pathlib import Path
import shutil
import cv2
import numpy as np
from prepare_competition_replay import digest, rgb565

ROOT = Path(__file__).resolve().parents[1]
CHANNELS = ('CAM_FRONT', 'CAM_BACK_LEFT', 'CAM_BACK_RIGHT', 'CAM_BACK')


def export(scene, dataset, source_root, output_root):
    source = source_root / f'scene-{scene}'
    metadata = json.loads((source / 'manifest.json').read_text())
    rows = json.loads((source / 'frame_timestamps.json').read_text())
    if len(rows) != metadata['output_frames'] or metadata['output_fps'] != 12:
        raise ValueError('Unexpected original export timeline')
    destination = output_root / f'real_{scene}'
    if destination.exists():
        raise FileExistsError(f'Preserved existing output: {destination}')
    destination.mkdir(parents=True)
    files = []
    try:
        for ch in range(4):
            files.append((destination / f'ch{ch}.rgb565').open('xb'))
        for index, row in enumerate(rows):
            if row['frame'] != index:
                raise ValueError('Nonconsecutive export indices')
            views = []
            for ch, camera in enumerate(CHANNELS):
                path = (dataset / row['sources'][camera]['filename']).resolve()
                if not path.is_relative_to(dataset.resolve()):
                    raise ValueError('Source image outside dataset')
                image = cv2.imread(str(path))
                if image is None or image.shape[1] * 9 != image.shape[0] * 16:
                    raise ValueError(f'Unreadable/non-16:9 source: {path}')
                image = cv2.resize(image, (1280, 720), interpolation=cv2.INTER_AREA)
                files[ch].write(rgb565(image))
                views.append(image)
            if index in {0, len(rows)//2, len(rows)-1}:
                cv2.imwrite(str(destination / f'preview_{index:05d}.jpg'), np.vstack((np.hstack(views[:2]), np.hstack(views[2:]))))
            if index % 100 == 0:
                print(f'EXPORT REAL {scene}: {index}/{len(rows)}', flush=True)
    finally:
        for f in files:
            f.close()
    # Reuse already fully decoded videos; raw transport is generated from original JPEG, not MP4.
    for ch, camera in enumerate(CHANNELS):
        video = source / f'{camera}.mp4'
        recorded = next(v for v in metadata['videos'] if Path(v['path']).name == video.name)
        if digest(video) != recorded['sha256']:
            raise ValueError('Original MP4 checksum changed')
        shutil.copyfile(video, destination / f'ch{ch}_{camera}.mp4')
    shutil.copyfile(source / 'frame_timestamps.json', destination / 'frame_timestamps.json')
    (destination / 'replay.env').write_text(f'REPLAY_FRAMES={len(rows)}\nREPLAY_FPS=12\nREPLAY_SCENE=real_{scene}\n', encoding='ascii')
    info = {'scene': f'real_{scene}', 'kind': 'real road', 'fps': 12, 'frames': len(rows),
            'duration_s': len(rows)/12, 'source': 'https://www.nuscenes.org/data/v1.0-mini.tgz',
            'source_location': metadata['source_location'], 'description': metadata['description'],
            'image_size': [1280, 720], 'pixel_format': 'RGB565 little endian', 'channel_views': CHANNELS,
            'exact_frame_index_match': False, 'exact_sensor_synchronization': False,
            'camera_heading_degrees_left_positive': metadata['camera_heading_degrees_left_positive'],
            'max_board_camera_span_us': metadata['max_board_camera_span_us'],
            'reused_source_frames': {ch:metadata['reused_source_frames'][ch] for ch in CHANNELS},
            'reference_not_model_input': True, 'competition_fully_covered': False,
            'pixel_labels_or_safety_labels_exported': False,
            'files': {p.name: {'bytes': p.stat().st_size, 'sha256': digest(p)} for p in destination.iterdir() if p.is_file()}}
    (destination / 'manifest.json').write_text(json.dumps(info, indent=2) + '\n')
    print(f'VERIFIED REAL {scene}: {len(rows)} frames per stream, original timestamps preserved', flush=True)
    return info


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dataset', type=Path, default=ROOT/'datasets/nuscenes_mini_v1')
    parser.add_argument('--source', type=Path, default=ROOT/'runs/multiview_test_data_20261009')
    parser.add_argument('--output', type=Path, default=ROOT/'runs/competition_replay_data_20261009')
    args = parser.parse_args()
    results = [export(scene, args.dataset, args.source, args.output) for scene in ('0796', '0061', '1077')]
    target = ROOT/'maintenance/competition_replay_20261009/real_export_receipt.json'
    target.write_text(json.dumps(results, indent=2) + '\n')


if __name__ == '__main__':
    main()
