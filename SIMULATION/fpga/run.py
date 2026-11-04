"""ModelSim regression against the delivered RTL; output stays in a new run folder."""
from pathlib import Path
import argparse
import datetime
import json
import struct
import subprocess
import zlib


def main():
    here = Path(__file__).resolve().parent
    root = here.parents[1]
    parser = argparse.ArgumentParser()
    parser.add_argument('--sim-dir', type=Path, default=Path('D:/modeltech64_2020.4/win64'))
    parser.add_argument('--full', action='store_true', help='Also run long asynchronous-input/DDR/pixel scoreboard for both input sizes')
    args = parser.parse_args()
    suffix = '.exe' if (args.sim_dir / 'vsim.exe').is_file() else ''
    for tool in ('vlib', 'vlog', 'vsim'):
        if not (args.sim_dir / (tool + suffix)).is_file():
            raise FileNotFoundError('Set --sim-dir to ModelSim binaries')
    out = here / 'output' / datetime.datetime.now().strftime('%Y%m%d_%H%M%S_%f')
    out.mkdir(parents=True, exist_ok=False)
    rtl = sorted((root / 'FPGA/project/rtl').glob('*.v'))
    results = []

    def run(label, bench, expected, baseline=False, full_mode=None):
        work = out / label
        work.mkdir()
        sources = [here / 'fixtures/network_port_baseline.v' if baseline and p.name == 'network_port.v' else p for p in rtl]
        commands = [
            [args.sim_dir / ('vlib' + suffix), 'work'],
            [args.sim_dir / ('vlog' + suffix), '-sv', '+define+SIMULATION', '+define+FAST_IO_DELAY', *sources,
             here / 'static_input_delay.sv', here / (bench + '.sv')],
            [args.sim_dir / ('vsim' + suffix), '-c', 'work.' + bench,
             *(['-gTHIRD_HD1080=' + str(full_mode)] if full_mode is not None else []), '-do', 'run -all; quit -f']]
        output = ''
        for i, command in enumerate(commands):
            r = subprocess.run([str(x) for x in command], cwd=work, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, text=True, errors='replace', timeout=600 if full_mode is not None else 90)
            output = r.stdout
            (work / (str(i) + '.log')).write_text(output, encoding='utf8')
            if i < 2 and (r.returncode or '** Error' in output):
                raise RuntimeError(output[-3000:])
            if i == 2 and not baseline and (r.returncode or '** Fatal:' in output or '** Error' in output):
                raise RuntimeError(output[-3000:])
        assert expected in output, output[-3000:]
        if baseline:
            assert '** Fatal:' in output and 'raster=1' in output
        result = {'name': label, 'status': 'PASS', 'scope': 'expected negative control' if baseline else 'RTL simulation'}
        if bench == 'tb_network_panorama':
            raw = bytes.fromhex((work / 'network_panorama.hex').read_text())
            p = raw[8:-4]
            assert len(raw) == 8274 and struct.unpack('<I', raw[-4:])[0] == zlib.crc32(p)
            assert p[42:46] == b'QV04' and struct.unpack('!H', p[36:38])[0] == 5200
            assert p[70:] == b'\x57\x13' * 4096
            words = struct.unpack('!10H', p[14:34])
            total = sum(words)
            while total >> 16:
                total = (total & 65535) + (total >> 16)
            assert total == 65535
            result['ethernet_fcs_ip_checksum_payload_verified'] = True
        results.append(result)
        print(label + ': PASS', flush=True)

    run('old_gate_negative_control', 'tb_park30', 'mode=1 raster=1', baseline=True)
    run('park30_current', 'tb_park30', 'PASS PARK odd/even all frames')
    run('panorama_packet_current', 'tb_network_panorama', 'PASS: real network_port')
    if args.full:
        for mode in (0, 1):
            run('full_pixel_scoreboard_' + str(mode), 'tb_panorama_full', 'PASS: CH1 rotated', full_mode=mode)
    report = {'status': 'PASS', 'tests': results, 'full_pixel_scoreboard_run': args.full,
              'physical_hardware_test': False, 'output': str(out)}
    (out / 'result.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf8')
    print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
