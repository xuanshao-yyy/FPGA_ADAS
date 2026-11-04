"""Unified entry for local demos and checks; see README.md."""
from pathlib import Path
import argparse
import runpy
import sys
import unittest

ROOT = Path(__file__).resolve().parent
COMMANDS = {
    'prepare-replay': 'prepare_competition_replay.py',
    'prepare-real-replay': 'prepare_real_replay.py',
    'compare-detectors': 'compare_detectors.py',
    'train-light': 'train_traffic_light.py',
    'train-light-selected': 'train_light_selected.py',
    'train-light-overnight': 'train_light_overnight.py',
    'light-rgb': 'run_light_rgb.py',
    'train-yolo26': 'train_yolo26.py',
    'detect': 'test_videos.py',
    'track': 'track_videos.py',
    'lane': 'run_lane_pretrained.py',
    'lane-twinlite': 'run_lane_twinlite.py',
    'prepare-lane4': 'prepare_lane4_data.py',
    'train-lane4': 'train_lane4.py',
    'train-lane4-queue': 'train_lane4_queue.py',
    'evaluate-lane4': 'evaluate_lane4.py',
    'prepare-lane-frames': 'prepare_lane_frames.py',
    'lane4': 'run_lane4.py',
    'lane-legacy': 'run_lane_assist.py',
    'audit-data': 'audit_lane_training_data.py',
    'prepare-data': 'prepare_datasets.py',
    'check': 'check_project.py',
}


def execute(script, arguments=None):
    """Forward to the actual implementation, retaining existing CLI arguments."""
    sys.path.insert(0, str(ROOT / 'code'))
    sys.argv = [str(ROOT / 'code' / script), *(sys.argv[1:] if arguments is None else arguments)]
    runpy.run_path(str(ROOT / 'code' / script), run_name='__main__')


def main():
    parser = argparse.ArgumentParser(description='ADAS project entry. Run from the project root; see README.md.')
    parser.add_argument('command', choices=[*COMMANDS, 'test'], help='Choose a demo, data tool or validation.')
    parser.add_argument('arguments', nargs=argparse.REMAINDER, help='Arguments forwarded to the selected script.')
    args = parser.parse_args()
    if args.command == 'test':
        sys.path.insert(0, str(ROOT / 'code'))
        suite = unittest.defaultTestLoader.discover(str(ROOT / 'tests'), pattern='test_*.py')
        result = unittest.TextTestRunner(verbosity=2).run(suite)
        raise SystemExit(0 if result.wasSuccessful() else 1)
    execute(COMMANDS[args.command], args.arguments)


if __name__ == '__main__':
    main()
