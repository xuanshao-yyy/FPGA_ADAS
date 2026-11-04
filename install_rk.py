"""Explicit, non-overwriting install into a new RK directory. Default is check only."""
from pathlib import Path
import argparse
import os
import shutil
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--install', action='store_true')
    parser.add_argument('--target', type=Path, default=Path('/userdata/test_auto_drive/rk3568_final_20261103'))
    args = parser.parse_args()
    root = Path(__file__).resolve().parent
    subprocess.run([sys.executable, str(root / 'verify.py')], check=True)
    if os.name != 'posix' or os.uname().machine != 'aarch64':
        raise RuntimeError('Run on RK ARM64 Linux; PC extraction does not install or start RK')
    target = args.target.absolute()
    if target.parent.resolve() != Path('/userdata/test_auto_drive').resolve():
        raise ValueError('Use a new direct child of /userdata/test_auto_drive')
    if target.exists() or target.is_symlink():
        raise FileExistsError('Preserve existing directory: ' + str(target))
    source = root / 'RK'
    total = sum(p.stat().st_size for p in source.rglob('*') if p.is_file())
    if shutil.disk_usage(target.parent).free < total + 2 * 1024 ** 3:
        raise OSError('Need runtime size plus 2 GiB reserve; do not remove historical deployments')
    print('Target: ' + str(target) + '; bytes: ' + str(total))
    if not args.install:
        print('Check complete. Use --install to copy; no service has been changed.')
        return
    shutil.copytree(source, target, symlinks=False)
    for p in target.rglob('*.sh'):
        p.chmod(0o755)
    for name in ('rk_signal_control', 'rk_park_stream', 'rk_panorama_view', 'rk_mode_hub'):
        (target / name).chmod(0o755)
    state = target / 'results/product_runtime'
    state.mkdir(parents=True)
    for p in (target / 'initial_state').glob('*.json'):
        shutil.copy2(p, state / p.name)
    print('Installed. Stop any prior product before running:')
    print('cd ' + str(target) + ' && bash start_live_product.sh')


if __name__ == '__main__':
    main()
