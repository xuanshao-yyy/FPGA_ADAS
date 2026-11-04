"""Read bounded tails of this runtime's own operation/decision CSVs."""
from pathlib import Path
import csv
import io
import os
import math

FILES = ('user_operations.csv', 'driver_advice.csv', 'lane_rules.csv', 'scene_events.csv')


def tail_rows(path, limit=60, budget=65536):
    """Bound I/O even when a long run has accumulated a large CSV."""
    with Path(path).open('rb') as stream:
        header = stream.readline(4096)
        if not header.endswith(b'\n'):
            return []
        start = stream.tell()
        stream.seek(0, os.SEEK_END)
        end = stream.tell()
        offset = max(start, end-budget)
        stream.seek(offset)
        if offset > start:
            stream.readline()  # Ignore an incomplete first record.
        body = stream.read(budget)
    if body and not body.endswith(b'\n'):
        body = body.rsplit(b'\n', 1)[0]+b'\n' if b'\n' in body else b''
    rows = list(csv.DictReader(io.StringIO((header+body).decode('utf8', errors='replace'))))
    return rows[-limit:]


def collect(root, pid, previous=None):
    root = Path(root).resolve()
    run = None
    if pid:
        proc = Path('/proc')/str(int(pid))
        try:
            if (proc/'exe').resolve() == root/'rk_signal_control':
                for fd in (proc/'fd').iterdir():
                    try:
                        path = fd.resolve()
                        if path.name == 'user_operations.csv' and root/'results' in path.parents:
                            run = path.parent
                            break
                    except OSError:
                        pass
        except OSError:
            pass
    if run is None and previous:
        candidate = Path(previous).resolve()
        if root/'results' in candidate.parents and candidate.is_dir():
            run = candidate
    rows = []
    if run:
        for name in FILES:
            path = run/name
            if path.is_file():
                try:
                    source = tail_rows(path)
                except OSError:
                    continue
                latest_model_kept = False
                for row in reversed(source):
                    row['log'] = name
                    try:
                        row['monotonic_s'] = float(row.get('now_s', '-1'))
                    except (TypeError, ValueError):
                        continue
                    if not math.isfinite(row['monotonic_s']):
                        continue
                    if name == 'lane_rules.csv' and row.get('event') == 'MODEL_OBSERVATION':
                        if latest_model_kept:
                            continue
                        latest_model_kept = True  # One current diagnostic, not a frame-by-frame feed.
                    rows.append(row)
    rows.sort(key=lambda row: row['monotonic_s'], reverse=True)
    return dict(rows=rows[:100], run_directory=str(run) if run else '',
                note='单调秒按RK本次系统启动计时；操作应用不代表允许通行。原始模型观测保留在CSV。'), run
