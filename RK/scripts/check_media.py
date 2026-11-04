"""Reject wrong/truncated/replaced calibration clips before launching a scene."""
from pathlib import Path
import hashlib
import json
import sys

root = Path(__file__).resolve().parents[1]
scene = {'red': 'red_arrow', 'green': 'green_straight'}[sys.argv[1]]
path = root / 'data' / (scene + '.rgb565')
metadata = json.loads(path.with_suffix('.json').read_text(encoding='utf-8'))
assert path.stat().st_size == metadata['bytes'] == metadata['frames'] * 1280 * 720 * 2
assert metadata['width'] == 1280 and metadata['height'] == 720 and metadata['fps'] == 30
digest = hashlib.sha256()
with path.open('rb') as source:
    for block in iter(lambda: source.read(4 * 1024 * 1024), b''):
        digest.update(block)
assert digest.hexdigest() == metadata['sha256'], 'Media changed; scene-specific ROI mapping no longer verified'
print('PASS media identity: ' + scene)
