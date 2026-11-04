"""Windows overnight launcher: opt-in training, logs, duplicate-job guard and temporary wake request."""
import argparse
from contextlib import contextmanager
import ctypes
from datetime import datetime
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

from project_paths import PROJECT_ROOT as ROOT


def command(folder, args):
    return [sys.executable, '-X', 'utf8', '-u', str(ROOT/'run.py'), 'train-light-selected',
            '--stage', args.stage, '--epochs', '100', '--color-epochs', '100',
            '--patience', '15', '--color-patience', '10',
            '--batch', str(args.batch), '--color-batch', str(args.color_batch),
            '--workers', str(args.workers), '--output', str(folder/'training')]


def active_training():
    query = "$ErrorActionPreference='Stop'; $trainingIds=@(Get-CimInstance Win32_Process | Where-Object { $_.Name -match '^python(w)?\\.exe$' -and $_.CommandLine -match 'train-light-selected|train_light_selected\\.py' } | Select-Object -ExpandProperty ProcessId); ConvertTo-Json -InputObject $trainingIds -Compress"
    result = subprocess.run(['powershell.exe', '-NoProfile', '-NonInteractive', '-Command', query],
                            capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=20)
    if result.returncode:
        raise RuntimeError('Cannot verify existing training processes; refusing a second job. '+result.stderr.strip())
    value = json.loads(result.stdout.strip())
    if not isinstance(value, list) or any(not isinstance(pid, int) for pid in value):
        raise ValueError('Unexpected process-query response; no training launched')
    return value


@contextmanager
def singleton():
    from ctypes import wintypes
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.CreateMutexW.argtypes = [ctypes.c_void_p, wintypes.BOOL, wintypes.LPCWSTR]
    kernel.CreateMutexW.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    name = 'Local\\ADASLightNight_'+hashlib.sha256(str(ROOT).lower().encode()).hexdigest()[:20]
    handle = kernel.CreateMutexW(None, False, name)
    already_exists = ctypes.get_last_error() == 183
    if not handle:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        if already_exists:
            raise RuntimeError('Another overnight launcher is already active for this project')
        yield
    finally:
        kernel.CloseHandle(handle)


def run(args):
    if os.name != 'nt':
        raise RuntimeError('This overnight launcher supports Windows only')
    with singleton():
        pids = active_training()
        if pids:
            raise RuntimeError(f'Existing selected-data training PIDs {pids}. Stop it in its terminal with Ctrl+C before restarting. No process was terminated.')
        folder = ROOT/'runs'/('light_overnight_'+datetime.now().strftime('%Y%m%d_%H%M%S_%f'))
        folder.mkdir()
        cmd = command(folder, args)
        job = {'command': cmd, 'complete': False, 'started': False,
               'output': str(folder), 'training_directory': str(folder/'training'),
               'detector_limit': 100, 'color_limit': 100, 'patience': 15, 'color_patience': 10}
        status = folder/'job.json'
        def save():
            status.write_text(json.dumps(job, indent=2), encoding='utf-8')
        save()
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.SetThreadExecutionState.argtypes = [ctypes.c_uint]
        kernel.SetThreadExecutionState.restype = ctypes.c_uint
        awake = bool(kernel.SetThreadExecutionState(0x80000001))  # CONTINUOUS | SYSTEM_REQUIRED
        job['system_awake_request_succeeded'] = awake
        print(f'Logs and models: {folder}', flush=True)
        if not awake:
            print('Wake request failed; ensure Windows will stay awake.', flush=True)
        try:
            with (folder/'console.log').open('w', encoding='utf-8', buffering=1) as log:
                environment = dict(os.environ, PYTHONUTF8='1', PYTHONUNBUFFERED='1')
                with subprocess.Popen(cmd, cwd=ROOT, env=environment, stdout=subprocess.PIPE,
                                      stderr=subprocess.STDOUT, text=True, encoding='utf-8', errors='replace', bufsize=1) as child:
                    job.update(started=True, child_pid=child.pid)
                    save()
                    for line in child.stdout:
                        log.write(line)
                        print(line, end='', flush=True)
                    job['exit_code'] = child.wait()
            job['complete'] = job['exit_code'] == 0
            if not job['complete']:
                raise RuntimeError(f'Training exited with code {job["exit_code"]}; see {folder/"console.log"}')
        except BaseException as error:
            job['error'] = str(error)
            raise
        finally:
            save()
            if awake:
                kernel.SetThreadExecutionState(0x80000000)  # Restore normal sleep behavior.


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stage', choices=['all', 'detector', 'color'], default='all')
    parser.add_argument('--batch', type=int, default=8)
    parser.add_argument('--color-batch', type=int, default=64)
    parser.add_argument('--workers', type=int, default=2)
    parser.add_argument('--dry-run', action='store_true', help='Print command only: no training, process or power changes')
    args = parser.parse_args()
    if min(args.batch, args.color_batch) < 1 or args.workers < 0:
        parser.error('batch sizes must be positive; workers nonnegative')
    if args.dry_run:
        print(subprocess.list2cmdline(command(ROOT/'runs/light_overnight_DRY_RUN', args)))
        return
    run(args)


if __name__ == '__main__':
    main()
