"""Verify the delivery without importing hardware modules or writing runtime files."""
from pathlib import Path
import hashlib
import json


def main():
    root = Path(__file__).resolve().parent
    manifest = json.loads((root / 'SHA256.json').read_text(encoding='utf8'))
    failures = []
    for name, row in manifest.items():
        p = root / name
        if not p.resolve().is_relative_to(root):
            raise ValueError('Manifest leaves delivery root')
        if not p.is_file() or p.is_symlink():
            failures.append(name)
            continue
        h = hashlib.sha256()
        with p.open('rb') as f:
            for chunk in iter(lambda: f.read(1024 * 1024), b''):
                h.update(chunk)
        if p.stat().st_size != row['bytes'] or h.hexdigest() != row['sha256']:
            failures.append(name)
    if failures:
        raise RuntimeError('Missing or changed files: ' + ', '.join(failures))
    print('PASS SHA256: ' + str(len(manifest)) + ' files')


if __name__ == '__main__':
    main()
