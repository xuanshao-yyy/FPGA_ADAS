"""Upgrade this task's derived labels to eight-class partial supervision and add S2TLD."""
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import xml.etree.ElementTree as ET
import zipfile

from PIL import Image
import yaml

from project_paths import DATA_ARCHIVES
from train_traffic_light import DATA, START, check, digest, light_rows

NAMES = ['person', 'rider', 'car', 'bus', 'truck', 'bicycle', 'motorcycle', 'traffic_light']
HOLDOUT_SUFFIX = '/two/re_traffic_lights_6.25/images'


def voc_rows(contents):
    root = ET.fromstring(contents)
    width, height = int(root.findtext('size/width')), int(root.findtext('size/height'))
    if (width, height) != (1280, 720):
        raise ValueError(f'Unexpected S2TLD dimensions: {width}x{height}')
    rows, states = [], []
    for obj in root.findall('object'):
        color = obj.findtext('name').strip().lower().replace(' ', '_')
        if color not in ('red', 'green', 'yellow', 'off', 'wait_on'):
            raise ValueError(f'Unknown S2TLD class: {color}')
        # VOC integer coordinates are one-based inclusive; use zero-based half-open xyxy.
        x1, y1 = float(obj.findtext('bndbox/xmin'))-1, float(obj.findtext('bndbox/ymin'))-1
        x2, y2 = float(obj.findtext('bndbox/xmax')), float(obj.findtext('bndbox/ymax'))
        x1, y1, x2, y2 = max(0, x1), max(0, y1), min(width, x2), min(height, y2)
        row = f'7 {(x1+x2)/(2*width):.8f} {(y1+y2)/(2*height):.8f} {(x2-x1)/width:.8f} {(y2-y1)/height:.8f}'
        light_rows(row, 7)  # finite, nondegenerate normalized rectangle checks
        rows.append(row)
        states.append(dict(color=color, box_xyxy=[x1, y1, x2, y2],
                           difficult=obj.findtext('difficult', '0'), truncated=obj.findtext('truncated', '0')))
    return rows, states, str(PurePosixPath(root.findtext('path')).parent)


def upgrade():
    report = json.loads((DATA / 'manifest.json').read_text(encoding='utf-8'))
    if report['version'] == 2:
        check()
        print('S2TLD already prepared; nothing overwritten.', flush=True)
        return
    if report['version'] != 1:
        raise ValueError('Unknown derived dataset version; refusing to change it')
    check()
    archive = DATA_ARCHIVES / 's2tld_720x1280.zip'
    if not archive.is_file():
        raise FileNotFoundError('Run python code/download_s2tld.py first')
    source_info = json.loads(archive.with_suffix('.source.json').read_text(encoding='utf-8'))
    if archive.stat().st_size != source_info['bytes']:
        raise ValueError('S2TLD archive size changed')
    with archive.open('rb') as stream:
        sha = hashlib.file_digest(stream, 'sha256').hexdigest() if hasattr(hashlib, 'file_digest') else None
    if sha is None:
        value = hashlib.sha256()
        with archive.open('rb') as stream:
            for block in iter(lambda: stream.read(8*1024*1024), b''):
                value.update(block)
        sha = value.hexdigest()
    if sha != source_info['sha256']:
        raise ValueError('S2TLD archive hash changed')
    entries = []
    with zipfile.ZipFile(archive) as bundle:
        members = set(bundle.namelist())
        for member in sorted(n for n in members if n.endswith('.xml')):
            xml = bundle.read(member)
            rows, states, source_group = voc_rows(xml)
            image_member = member.replace('/Annotations/', '/JPEGImages/').rsplit('.', 1)[0]+'.jpg'
            if image_member not in members:
                raise FileNotFoundError(image_member)
            split = 'val' if source_group.endswith(HOLDOUT_SUFFIX) else 'train'
            group = member.split('/')[-3]
            filename = f'{group}__{PurePosixPath(image_member).name}'
            entries.append(dict(xml=xml, rows=rows, states=states, source_group=source_group,
                                member=image_member, split=split, filename=filename))
        groups = {}
        for entry in entries:
            key = entry['source_group']
            groups[key] = groups.get(key, 0)+1
        if len(groups) != 3 or not any(key.endswith(HOLDOUT_SUFFIX) for key in groups):
            raise ValueError('Unexpected source groups; do not randomly split adjacent frames')
        for split in ('train', 'val'):
            for kind in ('images', 'labels'):
                (DATA / kind / f's2tld_{split}').mkdir(parents=True, exist_ok=False)
        annotations = DATA / 'source_annotations/s2tld'
        annotations.mkdir(parents=True)
        states_file = (DATA / 's2tld_states.jsonl').open('w', encoding='utf-8')
        stats = {split: dict(images=0, objects=0, negative_images=0, colors={}) for split in ('train', 'val')}
        with states_file:
            for entry in entries:
                split = entry['split']
                image = DATA / 'images' / f's2tld_{split}' / entry['filename']
                label = DATA / 'labels' / f's2tld_{split}' / (image.stem+'.txt')
                xml_path = annotations / (image.stem+'.xml')
                pixels = bundle.read(entry['member'])  # ZIP CRC checked by ZipFile.read
                with Image.open(io.BytesIO(pixels)) as decoded:
                    if decoded.size != (1280, 720):
                        raise ValueError(f'Image/XML size mismatch: {image}')
                    decoded.verify()
                image.write_bytes(pixels)
                xml_path.write_bytes(entry['xml'])
                label.write_bytes(('\n'.join(entry['rows'])+('\n' if entry['rows'] else '')).encode('ascii'))
                stats[split]['images'] += 1
                stats[split]['objects'] += len(entry['rows'])
                stats[split]['negative_images'] += not entry['rows']
                for obj in entry['states']:
                    color = obj['color']
                    stats[split]['colors'][color] = stats[split]['colors'].get(color, 0)+1
                states_file.write(json.dumps(dict(image=image.relative_to(DATA).as_posix(),
                    source_group=entry['source_group'], lights=entry['states']), ensure_ascii=False)+'\n')
                report['records'].append(dict(image=image.relative_to(DATA).as_posix(),
                    label=label.relative_to(DATA).as_posix(), source_label=str(xml_path),
                    source_sha256=digest(xml_path), output_sha256=digest(label), source_kind='s2tld_voc',
                    supervision='traffic_light_only'))
    for group in ('bdd_train', 'bdd_val'):
        stats_group = report['stats'][group]
        stats_group['traffic_light_objects'] = stats_group.pop('objects')
        stats_group['no_light_images'] = stats_group.pop('negative_images')
        stats_group.update(objects=0, negative_images=0, objects_by_class={name: 0 for name in NAMES})
    for record in report['records']:
        if record.get('source_kind') == 's2tld_voc':
            continue
        source = Path(record['source_label']).read_bytes()
        is_bdd = record['source_class'] == 7
        if is_bdd:
            output = source  # restore all eight ORIGINAL labels in BDD images
            record['supervision'] = 'all_eight'
            stats_group = report['stats'][PurePosixPath(record['image']).parent.name]
            original_rows = source.decode('ascii').splitlines()
            stats_group['objects'] += len(original_rows)
            stats_group['negative_images'] += not original_rows
            for row in original_rows:
                stats_group['objects_by_class'][NAMES[int(row.split()[0])]] += 1
        else:
            rows = ['7 '+row.split(' ', 1)[1] for row in light_rows(source.decode('ascii'), 0)]
            output = ('\n'.join(rows)+('\n' if rows else '')).encode('ascii')
            record['supervision'] = 'traffic_light_only'
        (DATA / record['label']).write_bytes(output)
        record['output_sha256'] = hashlib.sha256(output).hexdigest()
    names = dict(enumerate(NAMES))
    configs = {
        'traffic_light.yaml': (['images/bdd_train', 'images/bosch_train', 'images/s2tld_train'], 'images/bdd_val'),
        'traffic_light_smoke.yaml': ('smoke_train.txt', 'smoke_val.txt'),
        'bdd_eval.yaml': ('images/bdd_train', 'images/bdd_val'),
        'bosch_eval.yaml': ('images/bosch_train', 'images/bosch_val'),
        's2tld_eval.yaml': ('images/s2tld_train', 'images/s2tld_val'),
    }
    for filename, (train, val) in configs.items():
        (DATA / filename).write_text(yaml.safe_dump(dict(path=DATA.as_posix(), train=train, val=val, names=names),
                                                   sort_keys=False), encoding='utf-8')
    def smoke_paths(group, positive_count, negative_count):
        positives, negatives = [], []
        for label in sorted((DATA / 'labels' / group).glob('*.txt')):
            (positives if label.read_bytes() else negatives).append(label.stem)
        directory = DATA / 'images' / group
        image_paths = {p.stem: p for p in directory.iterdir()}
        # For BDD full labels, positivity here means any object; sufficient for an execution check.
        chosen = positives[:positive_count]+negatives[:negative_count]
        if len(chosen) < positive_count+negative_count:
            chosen += positives[positive_count:positive_count+negative_count-len(negatives[:negative_count])]
        return [image_paths[stem].as_posix() for stem in chosen]
    train = smoke_paths('bdd_train', 128, 0)+smoke_paths('bosch_train', 48, 16)+smoke_paths('s2tld_train', 64, 0)
    val = smoke_paths('bdd_val', 64, 0)
    for split, paths in (('train', train), ('val', val)):
        (DATA / f'smoke_{split}.txt').write_text('\n'.join(paths)+'\n', encoding='utf-8')
    report.update(version=2, scope='Eight-class v8 fine-tuning; partial sources supervise class 7 only.',
                  s2tld=dict(source=source_info, source_groups=groups, stats=stats,
                    split='Hold out the complete XML source-directory group ending '+HOLDOUT_SUFFIX,
                    independence='Source-directory grouping; no video IDs or cross-group scene independence guarantee',
                    coordinates='VOC one-based inclusive to zero-based half-open xyxy'),
                  inference='One standard YOLOv8n detector, eight original classes; no extra detector required',
                  validation='Model selection on complete BDD val only; partial-source evaluation filters class 7')
    report['stats'].update({f's2tld_{split}': value for split, value in stats.items()})
    (DATA / 'manifest.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps(dict(s2tld=stats, training_images=15000+4035+stats['train']['images']), indent=2), flush=True)
    check()


if __name__ == '__main__':
    upgrade()
