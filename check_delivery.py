"""Check final-package entry chain, origins, model copies and FPGA design inputs."""
from pathlib import Path
import argparse
import hashlib
import json
import re
import subprocess
import sys


def sha(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('delivery', type=Path)
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    root = args.delivery.resolve()
    subprocess.run([sys.executable, str(root / 'verify.py')], check=True)
    rk = root / 'RK'
    for name in ('rk_signal_control', 'rk_park_stream', 'rk_panorama_view', 'rk_mode_hub'):
        data = (rk / name).read_bytes()
        assert data[:4] == b'\x7fELF' and data[4:6] == b'\x02\x01'
        assert int.from_bytes(data[18:20], 'little') == 183, name
    models = json.loads((rk / 'model_contract.json').read_text(encoding='utf8'))
    for name, info in models['models'].items():
        assert sha(rk / 'models' / name) == info['sha256']
        assert sha(root / 'MODELS/runtime' / name) == info['sha256']
    assert sha(rk / 'parking_model.rknn') == models['parking_model_sha256']
    assert sha(rk / 'parking_model.rknn') == sha(root / 'MODELS/runtime/parking_model.rknn')
    web_count = 0
    for p in (rk / 'product/web').rglob('*'):
        if p.is_file():
            assert sha(p) == sha(root / 'PC/web' / p.relative_to(rk / 'product/web'))
            web_count += 1
    project = root / 'FPGA/project'
    pds = (project / 'QUAD_SFP0_SFP1_RGB565_JUMBO.pds').read_text(encoding='utf8')
    inputs = [name for name in re.findall(r'\(_file "([^"]+)"', pds)
              if name.startswith(('rtl/', 'vendor/', 'constraints/'))]
    assert inputs, 'No FPGA design inputs parsed'
    assert all((project / name).is_file() for name in inputs), [n for n in inputs if not (project / n).is_file()]
    # No main executable is launched. The existing launcher only prints resolved arguments.
    dry = subprocess.check_output([sys.executable, str(rk / 'scripts/start_live.py'), 'live', '--dry-run'], text=True)
    command = json.loads(dry)['command']
    for flag in ('--model', '--front-model', '--lane-config'):
        assert Path(command[command.index(flag) + 1]).is_file(), flag
    assert command[2:4] == ['live', 'interactive']
    # Fixed published measurements, not values edited for presentation.
    parking = json.loads((root / 'verification/parking_30fps_20261030/board_measurement.json').read_text())
    assert round(parking['machine_cpu_pct'], 2) == 31.12
    assert round(parking['wire']['fps'], 2) == 30.07
    place = (root / 'FPGA/reports/place_route.log').read_text(encoding='utf8', errors='replace')
    assert all(str(n) in place for n in (10374, 7069, 9658, 5387, 108))
    forbidden = [p for p in root.rglob('*') if p.is_file() and
        (p.name in {'access_token.txt', 'service.lock', '.env'} or p.suffix in {'.pem', '.key'})]
    assert not forbidden
    report = {'status': 'PASS', 'arm64_elf_files': 4, 'matched_browser_files': web_count,
        'fpga_existing_design_inputs': len(inputs), 'model_contract_hashes_match': True,
        'live_launcher_dry_run': True, 'existing_runtime_started': False,
        'performance_values_cross_checked': True, 'secrets_included': False}
    if args.report:
        args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf8')
    print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
