"""Record evidence only after timing, packet checks and HSST simulation pass."""
from pathlib import Path
import hashlib
import json
import re

ROOT = Path(__file__).resolve().parents[1]
PROJECT = ROOT.name


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def text(path):
    raw = path.read_bytes()
    if raw.startswith((b'\xff\xfe', b'\xfe\xff')):
        return raw.decode('utf-16')
    return raw.decode('utf-8', errors='replace')


def main():
    report = text(ROOT / 'report_timing/quad_dual_top.rtr')
    assert 'Design Summary : All Constraints Met.' in report
    bit = ROOT / f'{PROJECT}.sbit'
    assert bit.is_file() and sha(bit) == sha(ROOT / 'generate_bitstream/quad_dual_top.sbit')
    assert bit.stat().st_mtime >= (ROOT / 'report_timing/quad_dual_top.rtr').stat().st_mtime
    for name in ('hsst_simulation_20261012.txt', 'network_simulation_20261012.txt'):
        log = text(ROOT / 'docs' / name)
        assert '# PASS:' in log and not re.search(r'\*\* (Error|Fatal)', log), name
    receiver = text(ROOT / 'docs/rk_receiver_tests_20261012.txt')
    assert 'Ran 6 tests' in receiver and '\nOK' in receiver
    packets = json.loads((ROOT / 'tests/packet_verification_8192.json').read_text())
    assert packets['frames'] == 11 and packets['pixels_checked'] == 10137600
    pins = text(ROOT / 'place_route/quad_dual_top_prr.prt')
    for pin, signal in [('AB7', 'sfp_disable'), ('H12', 'sfp1_disable'), ('E16', 'sfp2_disable')]:
        assert re.search(r'^' + pin + r'\|.*\|' + signal + r'\|output\|YES', pins, re.M)
    evidence_pins = [line for line in pins.splitlines() if any('|' + s + '|' in line
                     for s in ('sfp_disable', 'sfp1_disable', 'sfp2_disable'))]
    (ROOT / 'docs/verified_sfp_pins.txt').write_text('\n'.join(evidence_pins) + '\n', encoding='utf-8')
    timing_excerpt = report[report.index('Performance Summary:'):report.index('Recovery Summary(Slow Corner):')]
    (ROOT / 'docs/timing_summary.txt').write_text(timing_excerpt, encoding='utf-8')
    sources = {}
    for folder in ('rtl', 'constraints', 'scripts', 'rk_receiver', 'tests'):
        for path in (ROOT / folder).rglob('*'):
            if path.is_file() and path.suffix in ('.v', '.sv', '.fdc', '.py', '.ps1'):
                sources[path.relative_to(ROOT).as_posix()] = sha(path)
    for path in [ROOT / 'vendor/sfp_hsst/sfp_hsst.idf', ROOT / 'vendor/sfp_hsst/sfp_hsst.v',
                 ROOT / 'vendor/sfp_hsst/rtl/ipml_hsst_sfp_hsst_wrapper_v1_4.v',
                 ROOT / f'{PROJECT}.pds', ROOT / 'build.ps1', ROOT / 'impl.tcl']:
        sources[path.relative_to(ROOT).as_posix()] = sha(path)
    info = {
        'date': '2026-10-12', 'project': PROJECT,
        'status': 'timing-qualified test candidate; physical SFP/RK3568/power validation pending',
        'baseline': 'QUAD_SFP0_SFP2_RGB565_JUMBO_R3',
        'git_branch': 'codex/sfp01-jumbo-20261012',
        'device': 'PGL50H FBG484 -6', 'tool': 'PDS 2022.2-SP6.4 build 146967',
        'gplace_seed': 3, 'timing': 'All Constraints Met; unchanged R3 timing budgets',
        'rejected_seed2': {'setup_slow_ns': -0.240, 'hold_fast_ns': -0.002, 'released': False},
        'bit_sha256': sha(bit),
        'lanes': {'SFP0': 0, 'SFP1': 1, 'SFP2': 'HSST disabled; TX_DISABLE=1; module supply remains connected'},
        'gpio': {'sfp_disable': 'AB7', 'sfp1_disable': 'H12', 'sfp2_disable': 'E16'},
        'line_rate_gbps': 1.25, 'reference_mhz': 125, 'encoding': '8b10b',
        'vendor_generation': 'IP generator PASS; lane-specific hidden port flags migrated as well as UI settings',
        'deskew': 'vendor cal_ipml_param.tcl defines lane-1 tap 2; lane-2 tap 1 is not copied',
        'video': {'width': 1280, 'height': 720, 'format': 'RGB565 little endian',
                  'HDMI_fps': 30, 'CAM1_fps': 15, 'HDMI_OUT': 'IN1 preview'},
        'endpoints': [
            {'channels': [0, 1], 'slot': 'SFP0', 'local_ip': '192.168.10.2', 'peer_ip': '192.168.10.1', 'ports': [5000, 5001]},
            {'channels': [2, 3], 'slot': 'SFP1', 'local_ip': '192.168.20.2', 'peer_ip': '192.168.20.1', 'ports': [5002, 5003]},
        ],
        'jumbo': {'image_bytes': 8192, 'udp_payload_bytes': 8220, 'packets_per_frame': 225, 'rk_mtu': 9000},
        'hsst_simulation': 'PASS: simultaneous physical lane 0/1 serial loopback, both 125MHz, PCS negotiation, 1024 checked bytes per lane',
        'hsst_simulator': 'ModelSim 2020.4 requires -novopt -suppress 12110 for supplied protected IP; vopt/+acc timeout recorded separately',
        'vendor_warnings': 'HSST protected model reports missing/too-few port connections; final simulation has 0 errors. Existing RAM WWCONF warnings remain.',
        'network_simulation': packets,
        'rk_script_unit_tests': '6 PASS: four-channel reorder/duplicates, missing, malformed/foreign/truncated, mixed source IDs, frame-ID wrap, read-only selective preflight',
        'hardware_test': 'not performed; do not reuse the abnormal hot module',
        'source_sha256': sources,
    }
    (ROOT / 'docs/verification.json').write_text(json.dumps(info, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    (ROOT / f'{PROJECT}.sbit.sha256').write_text(sha(bit) + '  ' + bit.name + '\n', encoding='ascii')
    print('Recorded qualified bit:', sha(bit))


if __name__ == '__main__':
    main()
