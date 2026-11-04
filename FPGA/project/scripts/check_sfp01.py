#!/usr/bin/env python3
"""Check the lane migration before synthesis; optional baseline SHA comparison."""
from pathlib import Path
import argparse
import hashlib
import json
import re
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]


def parameters(path):
    return {p.findtext('name'): p.findtext('value')
            for p in ET.parse(path).findall('.//param')}


def hardware_parameters(path):
    return {k: v.strip() for k, v in
            re.findall(r'\.(\w+)\s*\(([^\n]*)\)', path.read_text(encoding='utf-8'))}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path)
    args = parser.parse_args()
    top = (ROOT / 'rtl/quad_dual_top.v').read_text(encoding='utf-8')
    vendor = (ROOT / 'vendor/sfp_hsst/sfp_hsst.v').read_text(encoding='utf-8')
    for port in ['o_txlane_done_1', 'o_rxlane_done_1', 'i_p_l1rxp',
                 'i_p_l1rxn', 'o_p_l1txp', 'o_p_l1txn',
                 'i_p_tx1_clk_fr_core', 'i_p_rx1_clk_fr_core', 'i_txd_1']:
        assert '.' + port + '(' in top and re.search(r'\b' + port + r'\b', vendor), port
    assert 'sfp2_rxp' not in top
    assert "assign sfp2_disable = 1'b1;" in top
    assert 'output         o_txlane_done_2' not in vendor
    params = parameters(ROOT / 'vendor/sfp_hsst/sfp_hsst.idf')
    assert [params[f'CH{i}_EN'] for i in range(4)] == ['Fullduplex', 'Fullduplex', 'DISABLE', 'DISABLE']
    assert [params[f'TX{i}_EN'] for i in range(4)] == ['1', '1', '0', '0']
    assert [params[f'RX{i}_EN'] for i in range(4)] == ['1', '1', '0', '0']
    for lane in (0, 1):
        assert params[f'CH{lane}_TX_RATE'] == params[f'CH{lane}_RX_RATE'] == '1.25'
        assert params[f'CH{lane}_TX_ENCODER'] == params[f'CH{lane}_RX_DECODER'] == '8B10B'
    locations = {}
    fdc = (ROOT / 'constraints/board.fdc').read_text(encoding='utf-8')
    for port, loc in re.findall(r'define_attribute \{p:(.*?)\} \{PAP_IO_LOC\} \{(.*?)\}', fdc):
        assert loc not in locations, (loc, port, locations.get(loc))
        locations[loc] = port
    assert locations['H12'] == 'sfp1_disable'
    assert locations['E16'] == 'sfp2_disable'
    assert locations['AB7'] == 'sfp_disable'
    if args.baseline:
        baseline = args.baseline.resolve()
        old = hardware_parameters(baseline / 'vendor/sfp_hsst/rtl/ipml_hsst_sfp_hsst_wrapper_v1_4.v')
        new = hardware_parameters(ROOT / 'vendor/sfp_hsst/rtl/ipml_hsst_sfp_hsst_wrapper_v1_4.v')
        for key, value in old.items():
            if re.match(r'^(PMA|PCS)_(CH0_|CH2_|PLL0_)', key):
                migrated = re.sub(r'^(PMA|PCS)_CH2_', r'\1_CH1_', key)
                # Pango's template fixes the per-lane deskew taps to 3/2/1/0.
                # Keep lane 1's vendor-generated tap 2, rather than copying 1.
                if key == 'PCS_CH2_DELAY_SET':
                    assert value == '1' and new[migrated] == '2'
                    continue
                assert new.get(migrated) == value, (key, value, new.get(migrated))
        manifest = json.loads((ROOT / 'docs/baseline_sources.json').read_text(encoding='utf-8'))
        for name, digest in manifest['source_sha256'].items():
            assert hashlib.sha256((baseline / name).read_bytes()).hexdigest() == digest, name
            if name.startswith('rtl/') and name != 'rtl/quad_dual_top.v':
                assert (ROOT / name).read_bytes() == (baseline / name).read_bytes(), name
    print('PASS: lanes 0/1, 1.25Gbps 8b10b, correct H12/E16 pins, unique GPIO locations, SFP2 disabled.')
    if args.baseline:
        print('PASS: original source snapshot intact; 20 unchanged RTL files; electrical settings preserved, with vendor lane-1 deskew tap 2.')


if __name__ == '__main__':
    main()
