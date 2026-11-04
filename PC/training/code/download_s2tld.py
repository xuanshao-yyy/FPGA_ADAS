"""Download the author-linked S2TLD 720 archive with its published SHA256."""
import hashlib
import json
import time
from urllib.parse import quote

import requests
from project_paths import DATA_ARCHIVES

NAME = 'S2TLD（720x1280）.zip'
URL = 'https://huggingface.co/datasets/yangxue/S2TLD/resolve/main/' + quote(NAME)
SIZE = 1124547772
SHA256 = '303132bbb530dc877d82497941cad1ee005835196e8645a1e666edec0235bdb7'


def main():
    target = DATA_ARCHIVES / 's2tld_720x1280.zip'
    partial = target.with_suffix('.zip.part')
    DATA_ARCHIVES.mkdir(exist_ok=True)
    if not target.exists():
        offset = partial.stat().st_size if partial.exists() else 0
        with requests.get(URL, headers={'Range': f'bytes={offset}-'} if offset else {},
                          stream=True, timeout=(20, 45)) as response:
            response.raise_for_status()
            if offset and response.status_code != 206:
                raise RuntimeError('Server did not accept resume; existing partial file preserved.')
            last = time.monotonic()
            with partial.open('ab' if offset else 'wb') as stream:
                for block in response.iter_content(1024 * 1024):
                    stream.write(block)
                    offset += len(block)
                    if time.monotonic() - last > 10:
                        print(f'S2TLD download: {offset}/{SIZE} bytes ({offset/SIZE:.1%})', flush=True)
                        last = time.monotonic()
        if partial.stat().st_size != SIZE:
            raise RuntimeError('Download size mismatch; partial file preserved.')
        candidate = partial
    else:
        candidate = target
    digest = hashlib.sha256()
    with candidate.open('rb') as stream:
        for block in iter(lambda: stream.read(8 * 1024 * 1024), b''):
            digest.update(block)
    if candidate.stat().st_size != SIZE or digest.hexdigest() != SHA256:
        raise RuntimeError('Archive differs from author-linked file; refusing to use it.')
    if candidate == partial:
        partial.rename(target)
    target.with_suffix('.source.json').write_text(json.dumps(dict(url=URL, bytes=SIZE, sha256=SHA256,
        official_repository='https://github.com/Thinklab-SJTU/S2TLD'), indent=2), encoding='utf-8')
    print(f'Archive verified: {target}', flush=True)


if __name__ == '__main__':
    main()
