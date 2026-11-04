"""Download verified Bench2Drive clips and export four clean QV02 replay streams."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import gzip
import hashlib
import json
from pathlib import Path, PurePosixPath
import shutil
import tarfile
import time
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
CONFIG = ROOT / 'deployment/competition_replay_20261009/scenes.json'
FRAME_BYTES = 1280 * 720 * 2


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for block in iter(lambda: f.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def fetch(row, folder):
    folder.mkdir(parents=True, exist_ok=True)
    target = folder / row['archive']
    if target.exists():
        if target.stat().st_size != row['size'] or digest(target) != row['sha256']:
            raise ValueError(f'Existing archive failed checksum; preserved: {target}')
        print(f"VERIFIED {row['id']}: already downloaded", flush=True)
        return
    partial = target.with_suffix(target.suffix + '.part')
    for attempt in range(6):
        offset = partial.stat().st_size if partial.exists() else 0
        if offset > row['size']:
            raise ValueError(f'Oversized partial file, preserved: {partial}')
        if offset == row['size']:
            break
        request = urllib.request.Request(row['url'], headers={'Range': f'bytes={offset}-'} if offset else {})
        try:
            with urllib.request.urlopen(request, timeout=45) as response:
                if offset and (response.status != 206 or not response.headers.get('Content-Range', '').startswith(f'bytes {offset}-')):
                    raise ValueError('Server did not honor exact resume range; partial preserved')
                with partial.open('ab' if offset else 'wb') as out:
                    progress = time.monotonic()
                    while True:
                        block = response.read(1024 * 1024)
                        if not block:
                            break
                        out.write(block)
                        if time.monotonic() - progress > 10:
                            print(f"DOWNLOAD {row['id']}: {out.tell()}/{row['size']} bytes", flush=True)
                            progress = time.monotonic()
            if partial.stat().st_size == row['size']:
                break
        except (OSError, urllib.error.URLError) as error:
            print(f"RETRY {row['id']} attempt={attempt+1}: {error}", flush=True)
            time.sleep(min(2 ** attempt, 8))
    if not partial.exists() or partial.stat().st_size != row['size'] or digest(partial) != row['sha256']:
        raise ValueError(f'Download incomplete/checksum failed, preserved: {partial}')
    partial.rename(target)
    print(f"VERIFIED {row['id']}: {row['size']} bytes SHA256 matched official manifest", flush=True)


def safe_extract(archive, destination):
    if destination.exists():
        raise FileExistsError(f'Extraction destination exists, preserved: {destination}')
    destination.mkdir(parents=True)
    with tarfile.open(archive, 'r:gz') as source:
        for member in source:
            name = PurePosixPath(member.name)
            if name.is_absolute() or '..' in name.parts or any(':' in part or '\\' in part for part in name.parts):
                raise ValueError(f'Unsafe archive path: {member.name}')
            # Only camera RGB sequences + reference metadata, no lidar/depth dependencies.
            wanted = ('camera' in name.parts and any(p in ('rgb_front', 'rgb_back_left', 'rgb_back_right', 'rgb_back') for p in name.parts)) or 'anno' in name.parts
            if not wanted:
                continue
            if not member.isfile():
                if not member.isdir():
                    raise ValueError(f'Links/devices unsupported: {member.name}')
                continue
            path = destination.joinpath(*name.parts)
            path.parent.mkdir(parents=True, exist_ok=True)
            with source.extractfile(member) as src, path.open('xb') as dst:
                shutil.copyfileobj(src, dst)


def rgb565(image):
    import numpy as np
    rgb = image[:, :, ::-1].astype(np.uint16)
    return (((rgb[:, :, 0] >> 3) << 11) | ((rgb[:, :, 1] >> 2) << 5) | (rgb[:, :, 2] >> 3)).astype('<u2').tobytes()


def prepare(row, archives, raw_root, out_root):
    import cv2
    import numpy as np
    archive = archives / row['archive']
    if not archive.is_file() or archive.stat().st_size != row['size'] or digest(archive) != row['sha256']:
        raise ValueError(f'Archive not verified: {archive}')
    raw = raw_root / row['id']
    if not raw.exists():
        safe_extract(archive, raw)
    output = out_root / row['id']
    if output.exists():
        raise FileExistsError(f'Output exists, preserved: {output}; use a new --output directory')
    camera_names = ('rgb_front', 'rgb_back_left', 'rgb_back_right', 'rgb_back')
    streams = []
    for camera in camera_names:
        folders = list(raw.rglob(camera))
        if len(folders) != 1:
            raise ValueError(f'Expected one {camera} folder, found {len(folders)}')
        streams.append({int(p.stem): p for p in folders[0].glob('*.jpg')})
    ids = sorted(set(streams[0]).intersection(*(set(s) for s in streams[1:])))
    if not ids or any(set(s) != set(ids) for s in streams):
        raise ValueError('Missing/unmatched four-view frames; refusing silent nearest-frame pairing')
    if ids != list(range(ids[0], ids[-1] + 1)):
        raise ValueError('Nonconsecutive source frame IDs; timing needs explicit handling')
    refs = []
    annotation_files = {int(p.name.split('.')[0]): p for p in raw.rglob('*.json.gz') if 'anno' in p.parts}
    if set(annotation_files) != set(ids):
        raise ValueError('Reference annotations do not match image frames')
    output.mkdir(parents=True)
    files, videos = [], []
    try:
        for i, name in enumerate(camera_names):
            files.append((output / f'ch{i}.rgb565').open('xb'))
            videos.append(cv2.VideoWriter(str(output / f'ch{i}_{name}.mp4'), cv2.VideoWriter_fourcc(*'mp4v'), 10, (1280, 720)))
        if not all(v.isOpened() for v in videos):
            raise RuntimeError('MP4 writer unavailable')
        for index, fid in enumerate(ids):
            views = []
            for ch, stream in enumerate(streams):
                image = cv2.imread(str(stream[fid]))
                if image is None:
                    raise ValueError(f'Unreadable camera image: {stream[fid]}')
                if image.shape[1] * 9 != image.shape[0] * 16:
                    raise ValueError('Source is not 16:9; do not silently distort calibration')
                image = cv2.resize(image, (1280, 720), interpolation=cv2.INTER_AREA)
                files[ch].write(rgb565(image))
                videos[ch].write(image)
                views.append(image)
            with gzip.open(annotation_files[fid], 'rt', encoding='utf-8') as source:
                anno = json.load(source)
            boxes = anno.get('bounding_boxes', [])
            lights = [{'id': b.get('id'), 'state': b.get('state'), 'affects_ego': b.get('affects_ego'), 'distance': b.get('distance')} for b in boxes if b.get('class') == 'traffic_light']
            refs.append({'output_index': index, 'source_frame_id': fid, 'time_s': index / 10,
                         'speed': anno.get('speed'), 'command_near': anno.get('command_near'),
                         'next_command': anno.get('next_command'), 'weather': anno.get('weather'),
                         'traffic_lights': lights, 'actor_counts': {kind: sum(b.get('class') == kind for b in boxes) for kind in ('vehicle', 'walker', 'traffic_light')},
                         'visual_visibility_manually_checked': False})
            if index in {0, len(ids) // 2, len(ids) - 1}:
                cv2.imwrite(str(output / f'preview_{index:05d}.jpg'), np.vstack((np.hstack(views[:2]), np.hstack(views[2:]))))
            if index % 100 == 0:
                print(f"EXPORT {row['id']}: {index}/{len(ids)}", flush=True)
    finally:
        for f in files:
            f.close()
        for video in videos:
            video.release()
    for ch, name in enumerate(camera_names):
        cap = cv2.VideoCapture(str(output / f'ch{ch}_{name}.mp4'))
        count = 0
        while True:
            ok, image = cap.read()
            if not ok:
                break
            if image.shape[:2] != (720, 1280):
                raise ValueError('Encoded video dimensions differ')
            count += 1
        cap.release()
        if count != len(ids):
            raise ValueError('Encoded MP4 did not fully decode')
    (output / 'reference_events.json').write_text(json.dumps(refs, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    # This controlled config contains no input-provided executable shell text.
    (output / 'replay.env').write_text(f'REPLAY_FRAMES={len(ids)}\nREPLAY_FPS=10\nREPLAY_SCENE={row["id"]}\n', encoding='ascii')
    info = {'scene': row['id'], 'source': row, 'fps': 10, 'frames': len(ids), 'duration_s': len(ids) / 10,
            'image_size': [1280, 720], 'pixel_format': 'RGB565 little endian', 'channel_views': camera_names,
            'exact_frame_index_match': True, 'reference_not_model_input': True,
            'manually_reviewed': False, 'competition_fully_covered': False,
            'files': {p.name: {'bytes': p.stat().st_size, 'sha256': digest(p)} for p in output.iterdir() if p.is_file()}}
    (output / 'manifest.json').write_text(json.dumps(info, indent=2) + '\n', encoding='utf-8')
    print(f"VERIFIED EXPORT {row['id']}: {len(ids)} synchronized frame indices, four MP4 fully decoded", flush=True)
    return info


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stage', choices=('download', 'prepare', 'all'), default='all')
    parser.add_argument('--scenes', nargs='+', help='Selected scene IDs; default all six')
    parser.add_argument('--archives', type=Path, default=ROOT / 'data_archives/bench2drive_selected_20261009')
    parser.add_argument('--raw', type=Path, default=ROOT / 'datasets/bench2drive_selected_20261009')
    parser.add_argument('--output', type=Path, default=ROOT / 'runs/competition_replay_data_20261009')
    args = parser.parse_args()
    rows = json.loads(CONFIG.read_text(encoding='utf-8'))['scenes']
    if args.scenes:
        if set(args.scenes) - {r['id'] for r in rows}:
            parser.error('Unknown scene ID')
        rows = [r for r in rows if r['id'] in args.scenes]
    if args.stage in ('download', 'all'):
        with ThreadPoolExecutor(max_workers=2) as pool:
            list(pool.map(lambda row: fetch(row, args.archives), rows))
    if args.stage in ('prepare', 'all'):
        args.output.mkdir(parents=True, exist_ok=True)
        summaries = [prepare(row, args.archives, args.raw, args.output) for row in rows]
        receipt = ROOT / 'maintenance/competition_replay_20261009/export_receipt.json'
        receipt.parent.mkdir(parents=True, exist_ok=True)
        receipt.write_text(json.dumps(summaries, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
