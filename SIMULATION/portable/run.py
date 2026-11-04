"""Run three unmodified portable C++ logic tests; no camera, FPGA or RK needed."""
from pathlib import Path
import argparse
import datetime
import hashlib
import json
import os
import subprocess


def main():
    here = Path(__file__).resolve().parent
    root = here.parents[1]
    parser = argparse.ArgumentParser()
    parser.add_argument('--cxx', default='g++')
    args = parser.parse_args()
    out = here / 'output' / datetime.datetime.now().strftime('%Y%m%d_%H%M%S_%f')
    out.mkdir(parents=True, exist_ok=False)
    results = []
    for name in ('test_panorama_layout', 'test_display_job', 'test_parking_guard'):
        source = root / 'RK/tests' / (name + '.cpp')
        exe = out / (name + ('.exe' if os.name == 'nt' else ''))
        commands = [[args.cxx, '-std=c++17', '-O2', '-UNDEBUG', '-I' + str(root / 'RK/include'), str(source), '-o', str(exe)], [str(exe)]]
        for i, command in enumerate(commands):
            r = subprocess.run(command, cwd=root / 'RK', capture_output=True, text=True, errors='replace', timeout=90)
            (out / (name + '_' + str(i) + '.log')).write_text(r.stdout + r.stderr, encoding='utf8')
            if r.returncode:
                raise RuntimeError((r.stdout + r.stderr)[-3000:])
        results.append({'test': name, 'status': 'PASS', 'source_sha256': hashlib.sha256(source.read_bytes()).hexdigest()})
        print(name + ': PASS', flush=True)
    report = {'status': 'PASS', 'tests': results, 'compiler': args.cxx,
              'scope': 'Host logic assertions only; no NPU, GStreamer, physical display or accuracy test', 'output': str(out)}
    (out / 'result.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf8')
    print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
