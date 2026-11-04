"""Run four-class lane training sequentially, or continue after an existing Windows job."""
import argparse
import ctypes
from ctypes import wintypes
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import time

from project_paths import PROJECT_ROOT


def write_status(path, status):
    temporary = path.with_suffix('.tmp')
    temporary.write_text(json.dumps(status, indent=2), encoding='utf-8')
    temporary.replace(path)


def wait_for_process(pid):
    """Hold a handle to the original process, so PID reuse cannot change what we wait for."""
    if os.name != 'nt':
        raise RuntimeError('--wait-pid is supported on Windows only')
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    kernel.WaitForSingleObject.restype = wintypes.DWORD
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel.CloseHandle.restype = wintypes.BOOL
    handle = kernel.OpenProcess(0x00100000, False, pid)  # SYNCHRONIZE, read/wait only
    if not handle:
        if ctypes.get_last_error() == 87:  # Already exited / no such process
            return
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        while True:
            result = kernel.WaitForSingleObject(handle, 30000)
            if result == 0:
                return
            if result != 258:
                raise ctypes.WinError(ctypes.get_last_error())
            print(f'[QUEUE] Waiting for training PID {pid}; next model has not started', flush=True)
    finally:
        kernel.CloseHandle(handle)


def require_completed(output, data, stage, variant=None):
    """An exited process/last.pt alone does not establish successful completion."""
    report = json.loads((output/'report.json').read_text(encoding='utf-8'))
    settings = json.loads((output/'args.json').read_text(encoding='utf-8'))
    if (report.get('stage') != stage or settings.get('stage') != stage
            or report.get('completed_epochs', 0) < 1 or not report.get('checkpoint_reload_passed')
            or not math.isfinite(report.get('best_foreground_miou', float('nan')))
            or Path(settings['dataset']).resolve() != data.resolve()):
        raise RuntimeError(f'Previous training completion check failed: {output}')
    if variant is not None and (report.get('variant', 'small') != variant or settings.get('variant', 'small') != variant):
        raise RuntimeError(f'Completed run has the wrong model variant: {output}')
    for relative, expected in [('dataset.json', settings['dataset_config_sha256']),
                               ('train.jsonl', settings['train_index_sha256']),
                               ('val.jsonl', settings['val_index_sha256'])]:
        if hashlib.sha256((data/relative).read_bytes()).hexdigest() != expected:
            raise RuntimeError(f'Dataset changed since preceding training: {relative}')
    for name in ['best.pt', 'last.pt']:
        path = output/'weights'/name
        if not path.is_file() or path.stat().st_size == 0:
            raise RuntimeError(f'Missing completed checkpoint: {path}')
    return report


def new_model_output(variant, stage):
    name = f'lane4_{variant}_{"smoke" if stage == "smoke" else "bdd"}'
    output = PROJECT_ROOT/'runs'/name
    index = 1
    while output.exists():
        index += 1
        output = PROJECT_ROOT/'runs'/f'{name}-{index}'
    return output


def training_command(variant, args, output):
    command = [sys.executable, '-u', str(PROJECT_ROOT/'run.py'), 'train-lane4',
               '--stage', args.stage, '--variant', variant, '--data', str(args.data),
               '--device', args.device, '--output', str(output)]
    for key in ['epochs', 'batch', 'workers']:
        value = getattr(args, key)
        if value is not None:
            command.extend([f'--{key}', str(value)])
    return command


def execute_training(command, status, status_path):
    child = subprocess.Popen(command, cwd=PROJECT_ROOT)
    status['child_pid'] = child.pid
    write_status(status_path, status)
    try:
        code = child.wait()
        if code:
            raise RuntimeError(f'Training exited with code {code}; remaining models will not start')
    finally:
        if child.poll() is None:
            child.terminate()
            try:
                child.wait(timeout=10)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait()
        status['child_pid'] = None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--models', nargs='+', choices=['small', 'nano'], default=None)
    parser.add_argument('--after-run', type=Path, help='Existing training result directory to finish first')
    parser.add_argument('--wait-pid', type=int, help='Existing training Python PID; requires --after-run')
    parser.add_argument('--stage', choices=['train', 'smoke'], default='train')
    parser.add_argument('--data', type=Path, default=PROJECT_ROOT/'datasets/lane4_bdd_v1')
    parser.add_argument('--epochs', type=int)
    parser.add_argument('--batch', type=int)
    parser.add_argument('--workers', type=int)
    parser.add_argument('--device', choices=['cuda:0', 'cpu'], default='cuda:0')
    args = parser.parse_args()
    if args.wait_pid is not None and (args.wait_pid <= 0 or args.after_run is None):
        parser.error('Positive --wait-pid requires --after-run')
    if any(v is not None and v <= 0 for v in [args.epochs, args.batch]) or (args.workers is not None and args.workers < 0):
        parser.error('epochs/batch > 0; workers >= 0')
    args.models = args.models or (['nano'] if args.after_run else ['small', 'nano'])
    if len(set(args.models)) != len(args.models):
        parser.error('Do not list the same model twice')
    args.data = args.data.resolve()
    if args.after_run:
        args.after_run = args.after_run.resolve()
        previous_settings = json.loads((args.after_run/'args.json').read_text(encoding='utf-8'))
        if Path(previous_settings['dataset']).resolve() != args.data or previous_settings['stage'] != args.stage:
            parser.error('Existing job uses a different dataset or training stage')
    output = PROJECT_ROOT/'runs'/f'lane4_queue_{time.strftime("%Y%m%d_%H%M%S")}_{os.getpid()}'
    lock = PROJECT_ROOT/'runs/.lane4_queue.lock'
    with lock.open('x', encoding='utf-8') as file:
        json.dump(dict(pid=os.getpid(), queue=str(output), after_run=str(args.after_run), models=args.models), file)
    status = dict(pid=os.getpid(), models=args.models, stage=args.stage, dataset=str(args.data),
                  state='starting', after_run=str(args.after_run) if args.after_run else None,
                  wait_pid=args.wait_pid, child_pid=None, jobs=[])
    status_path = output/'queue.json'
    try:
        output.mkdir(exist_ok=False)
        write_status(status_path, status)
        print(f'[QUEUE] Status: {status_path}', flush=True)
        if args.after_run:
            status['state'] = 'waiting'
            write_status(status_path, status)
            print(f'[QUEUE] Finish existing job first: {args.after_run}; PID={args.wait_pid}', flush=True)
            if args.wait_pid:
                wait_for_process(args.wait_pid)
            require_completed(args.after_run, args.data, args.stage)
        for variant in args.models:
            destination = new_model_output(variant, args.stage)
            status['state'] = 'training'
            job = dict(variant=variant, output=str(destination), state='training')
            status['jobs'].append(job)
            write_status(status_path, status)
            print(f'[QUEUE] Starting {variant}: {destination}', flush=True)
            execute_training(training_command(variant, args, destination), status, status_path)
            report = require_completed(destination, args.data, args.stage, variant)
            job.update(state='completed', completed_epochs=report['completed_epochs'],
                       best_foreground_miou=report['best_foreground_miou'])
            write_status(status_path, status)
        status['state'] = 'completed'
        print('[QUEUE] All requested models completed', flush=True)
    except BaseException as error:
        status.update(state='failed', error=f'{type(error).__name__}: {error}')
        if status['jobs'] and status['jobs'][-1]['state'] == 'training':
            status['jobs'][-1]['state'] = 'failed'
        print(f'[QUEUE] STOPPED: {status["error"]}', flush=True)
        raise
    finally:
        if output.is_dir():
            write_status(status_path, status)
        lock.unlink(missing_ok=True)


if __name__ == '__main__':
    main()
