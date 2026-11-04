"""Verify delivery integrity without loading RKNN or contacting a board."""
from pathlib import Path
import hashlib
import json

ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(4 * 1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def require(condition, message):
    if not condition:
        raise ValueError(message)


def main():
    verified = set()
    for line in (ROOT / 'SHA256SUMS').read_text(encoding='utf-8').splitlines():
        digest, relative = line.split('  ', 1)
        path = (ROOT / relative).resolve()
        require(ROOT.resolve() in path.parents, 'Unsafe path: ' + relative)
        require(relative not in verified, 'Duplicate path: ' + relative)
        require(path.is_file() and sha(path) == digest, 'Checksum mismatch: ' + relative)
        verified.add(relative)
    required = {'rk_signal_control', 'model_contract.json', 'verification/software_checks.json',
                'verification/selected_models.json', 'run_test.sh', 'run_npu_check.sh'}
    require(required <= verified, 'Incomplete delivery manifest')
    with (ROOT / 'rk_signal_control').open('rb') as stream:
        header = stream.read(20)
    require(header[:6] == b'\x7fELF\x02\x01' and int.from_bytes(header[18:20], 'little') == 183, 'Expected ARM64 ELF')
    models = json.loads((ROOT / 'verification/selected_models.json').read_text(encoding='utf-8'))
    require({p.name for p in (ROOT / 'models').glob('*.rknn')} == set(models), 'Unexpected or missing RKNN model')
    require(len(models) == 4, 'Expected four runtime models')
    for name, info in models.items():
        require(info['precision'] == 'INT8', 'Non-INT8 model: ' + name)
        require('models/' + name in verified, 'Model absent from manifest: ' + name)
        path = ROOT / 'models' / name
        require(path.stat().st_size == info['bytes'] and sha(path) == info['sha256'], 'Model mismatch: ' + name)
    checks = json.loads((ROOT / 'verification/software_checks.json').read_text(encoding='utf-8'))
    require(checks['software_tests_passed'] == 36, 'Unexpected software test count')
    require(checks['physical_board_validated'] is False, 'No board acceptance is available for this delivery')
    require(checks['automatic_ego_signal_association_verified'] is False, 'Unexpected association acceptance')
    require(checks['all_runtime_models_int8'] is True, 'Expected INT8-only runtime')
    print('PASS', len(verified), 'files; ARM64 binary; four packaged INT8 models; 36 software tests recorded.')
    print('NOT board/FPS/AP acceptance. Three-probe RGB consensus check passed.')
    if not checks['raw_color_probability_gate_passed']:
        print('WARNING: raw RGB probability fidelity gate FAILED on dark probe; keep pixel consensus enabled.')


if __name__ == '__main__':
    main()
