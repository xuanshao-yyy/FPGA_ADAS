"""Create a traffic-light YOLO annotation pool and preserve D2-City source labels."""
import argparse
import csv
import hashlib
import json
import math
import os
import shutil
import xml.etree.ElementTree as ET
from collections import Counter
from pathlib import Path

os.environ.setdefault('OPENCV_FFMPEG_THREADS', '1')
import cv2

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / 'maintenance' / 'd2city_labeling_20261021'
CLASSES = ['car', 'van', 'bus', 'truck', 'person', 'bicycle', 'motorcycle',
           'open-tricycle', 'closed-tricycle', 'forklift', 'large-block', 'small-block']


def split_for_video(video):
    return 'val' if int(hashlib.sha256(video.encode()).hexdigest()[:8], 16) % 5 == 0 else 'train'


def yolo_box(box, width, height):
    values = [float(box.get(k)) for k in ('xtl', 'ytl', 'xbr', 'ybr')]
    if not all(math.isfinite(v) for v in values) or width <= 0 or height <= 0:
        raise ValueError('Non-finite box or invalid image dimensions')
    x1, y1, x2, y2 = values
    x1, x2 = max(0., min(width, x1)), max(0., min(width, x2))
    y1, y2 = max(0., min(height, y1)), max(0., min(height, y2))
    if x2 <= x1 or y2 <= y1:
        raise ValueError('Box is empty after clipping to image boundaries')
    return ((x1 + x2) / (2 * width), (y1 + y2) / (2 * height),
            (x2 - x1) / width, (y2 - y1) / height)


def parse_annotations(path):
    root = ET.parse(path).getroot()
    width, height = int(root.findtext('meta/width')), int(root.findtext('meta/height'))
    frames, skipped = {}, []
    for track in root.findall('track'):
        label = track.get('label')
        for box in track.findall('box'):
            frame = int(box.get('frame'))
            frames.setdefault(frame, [])
            if box.get('outside') == '1':
                continue
            if label not in CLASSES:
                skipped.append({'frame': frame, 'label': label, 'reason': 'not in official twelve classes'})
                continue
            values = yolo_box(box, width, height)
            frames[frame].append(str(CLASSES.index(label)) + ' ' + ' '.join(f'{v:.8f}' for v in values))
    return width, height, frames, skipped


def prepare(args):
    project = args.project.resolve()
    if project.exists():
        raise FileExistsError(f'Refusing to overwrite an annotation project: {project}')
    receipt = json.loads((WORK / 'subset_receipt.json').read_text(encoding='utf-8'))
    if not receipt['files']:
        raise ValueError('No downloaded videos in receipt')
    for record in receipt['files']:
        video = ROOT / record['path']
        if not video.is_file() or not (args.annotations / (video.stem + '.xml')).is_file():
            raise FileNotFoundError(f'Missing original video or XML: {video.stem}')
    for sub in ['images', 'labels', 'source_xml', 'reviewed',
                'reference_yolo13/images/train', 'reference_yolo13/images/val',
                'reference_yolo13/labels/train', 'reference_yolo13/labels/val']:
        (project / sub).mkdir(parents=True, exist_ok=True)
    (project / 'classes.txt').write_text('traffic_light\n', encoding='utf-8')
    (project / 'labels' / 'classes.txt').write_text('traffic_light\n', encoding='utf-8')
    ref = project / 'reference_yolo13'
    (ref / 'classes.txt').write_text('\n'.join(CLASSES + ['traffic_light']) + '\n', encoding='utf-8')
    for split in ['train', 'val']:
        (ref / 'labels' / split / 'classes.txt').write_text('\n'.join(CLASSES + ['traffic_light']) + '\n', encoding='utf-8')
    (ref / 'label_schema.yaml').write_text('names:\n' + ''.join(f'  {i}: {name}\n' for i, name in enumerate(CLASSES + ['traffic_light'])), encoding='utf-8')
    (project / 'data.yaml').write_text(
        '# Pending annotation. Run finalize only after marking reviewed images.\n'
        f'path: {project.as_posix()}\ntrain: reviewed/train.txt\nval: reviewed/val.txt\nnames:\n  0: traffic_light\n', encoding='utf-8')
    cv2.setNumThreads(1)
    rows, videos, skipped_all = [], [], []
    for record in receipt['files']:
        path = ROOT / record['path']
        xml = args.annotations / (path.stem + '.xml')
        width, height, annotated, skipped = parse_annotations(xml)
        skipped_all.extend(dict(video=path.stem, **entry) for entry in skipped)
        shutil.copy2(xml, project / 'source_xml' / xml.name)
        if hashlib.sha256(path.read_bytes()).hexdigest() != record['sha256']:
            raise ValueError(f'Source SHA256 mismatch: {path}')
        cap = cv2.VideoCapture(str(path))
        fps = cap.get(cv2.CAP_PROP_FPS)
        total = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
        if not cap.isOpened() or fps <= 0 or total <= 0:
            raise ValueError(f'Invalid source video: {path}')
        if (int(cap.get(3)), int(cap.get(4))) != (width, height):
            raise ValueError(f'XML/video dimensions disagree: {path}')
        if annotated and max(annotated) >= total:
            raise ValueError(f'XML frame outside video: {path}')
        chosen = set(annotated)
        chosen.update(int(round(t * fps)) for t in range(int(math.ceil(total / fps))) if int(round(t * fps)) < total)
        decoded, count = 0, 0
        split = split_for_video(path.stem)
        while True:
            ok, frame = cap.read()
            if not ok:
                break
            if decoded in chosen:
                stem = f'{path.stem}_f{decoded:06d}'
                image_path = project / 'images' / (stem + '.jpg')
                if not cv2.imwrite(str(image_path), frame, [cv2.IMWRITE_JPEG_QUALITY, 95]):
                    raise OSError(f'Image write failed: {image_path}')
                (project / 'labels' / (stem + '.txt')).write_text('', encoding='utf-8')
                has_reference = decoded in annotated
                if has_reference:
                    ref_image = ref / 'images' / split / image_path.name
                    shutil.copy2(image_path, ref_image)
                    lines = annotated[decoded]
                    (ref / 'labels' / split / (stem + '.txt')).write_text('\n'.join(lines) + ('\n' if lines else ''), encoding='utf-8')
                rows.append({'image': image_path.name, 'video': path.stem, 'frame': decoded,
                             'time_s': f'{decoded/fps:.6f}', 'split': split,
                             'source_12class_reference': int(has_reference), 'status': 'pending'})
                count += 1
            decoded += 1
        cap.release()
        if decoded != total or count != len(chosen):
            raise ValueError(f'Incomplete decode/extraction: {path}: {decoded}/{total}, {count}/{len(chosen)}')
        videos.append({'video': path.stem, 'fps': fps, 'width': width, 'height': height,
                       'frames': total, 'decoded_frames': decoded, 'sampled_images': count,
                       'reference_keyframes': len(annotated), 'split': split})
        print(f'{len(videos)}/{len(receipt["files"])} {path.stem}: {count} images, {len(annotated)} reference frames', flush=True)
    with (project / 'annotation_queue.csv').open('w', newline='', encoding='utf-8-sig') as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    summary = {'project': str(project), 'videos': videos, 'images': len(rows),
               'reference_keyframes': sum(v['reference_keyframes'] for v in videos),
               'split_images': dict(Counter(row['split'] for row in rows)),
               'skipped_source_annotations': skipped_all, 'annotation_status': 'pending',
               'traffic_light_class_id': 0, 'reference_traffic_light_class_id': 12,
               'training_ready': False, 'sampling': 'one frame per second plus explicitly annotated keyframes; original resolution'}
    (project / 'inventory.json').write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding='utf-8')
    (WORK / 'prepared_summary.json').write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding='utf-8')
    shutil.copy2(WORK / 'ANNOTATE.md', project / 'README.md')
    print(json.dumps({k: summary[k] for k in ['images', 'reference_keyframes', 'split_images', 'training_ready']}), flush=True)


def validate_light_label(path):
    lines = [line for line in path.read_text(encoding='utf-8-sig').splitlines() if line.strip()]
    for line in lines:
        fields = line.split()
        if len(fields) != 5 or fields[0] != '0':
            raise ValueError(f'Expected traffic_light class 0 and four coordinates: {path}')
        x, y, width, height = map(float, fields[1:])
        if not all(math.isfinite(v) for v in (x, y, width, height)) or not (0 < width <= 1 and 0 < height <= 1):
            raise ValueError(f'Invalid normalized label: {path}')
        if x-width/2 < -1e-6 or y-height/2 < -1e-6 or x+width/2 > 1+1e-6 or y+height/2 > 1+1e-6:
            raise ValueError(f'Box outside image: {path}')
    return len(lines)


def finalize(args):
    project = args.project.resolve()
    with (project / 'annotation_queue.csv').open(encoding='utf-8-sig', newline='') as stream:
        rows = list(csv.DictReader(stream))
    selected = [r for r in rows if r['status'] in {'positive', 'negative'}]
    if not selected:
        raise ValueError('No manually reviewed positive/negative rows; pending is not a negative image')
    groups = {'train': [], 'val': []}
    positives = 0
    for row in selected:
        image = project / 'images' / row['image']
        count = validate_light_label(project / 'labels' / (image.stem + '.txt'))
        if (row['status'] == 'positive') != (count > 0):
            raise ValueError(f'Review status disagrees with label: {image.name}')
        if not image.is_file() or row['split'] != split_for_video(row['video']):
            raise ValueError('Missing image or modified video split')
        groups[row['split']].append(image.as_posix())
        positives += count > 0
    if not positives or not all(groups.values()):
        raise ValueError('Need reviewed positive examples and both train/val video groups')
    for split, images in groups.items():
        (project / 'reviewed' / (split + '.txt')).write_text('\n'.join(images) + '\n', encoding='utf-8')
    print(f'Reviewed export: train={len(groups["train"])}, val={len(groups["val"])}, positive_images={positives}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stage', choices=['prepare', 'finalize'], default='prepare')
    parser.add_argument('--project', type=Path, default=ROOT / 'datasets' / 'd2city_light_annotation_v1')
    parser.add_argument('--annotations', type=Path)
    args = parser.parse_args()
    if args.stage == 'prepare' and args.annotations is None:
        parser.error('--annotations is required for prepare')
    prepare(args) if args.stage == 'prepare' else finalize(args)


if __name__ == '__main__':
    main()
