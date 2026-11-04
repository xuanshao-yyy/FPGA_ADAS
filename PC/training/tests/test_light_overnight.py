"""Night queue command/guard tests. No subprocess training or power changes."""
import json
from pathlib import Path
import subprocess
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'code'))
import train_light_overnight as night
from train_light_selected import parse_args


class OvernightPolicy(unittest.TestCase):
    def test_default_still_checks_but_explicit_training_has_100_epoch_limits(self):
        args = parse_args([])
        self.assertEqual(args.stage, 'check')
        self.assertEqual((args.epochs, args.color_epochs, args.patience, args.color_patience), (100, 100, 15, 10))

    def test_night_command_is_sequential_all_with_a_new_output(self):
        args = SimpleNamespace(stage='all', batch=8, color_batch=64, workers=2)
        cmd = night.command(Path('F:/output with space/night'), args)
        self.assertEqual(cmd[0], sys.executable)
        self.assertEqual(cmd[cmd.index('--stage')+1], 'all')
        self.assertEqual(cmd[cmd.index('--epochs')+1], '100')
        self.assertEqual(cmd[cmd.index('--color-epochs')+1], '100')
        self.assertTrue(cmd[-1].endswith('training'))

    def test_existing_processes_are_returned_without_killing_them(self):
        with patch.object(night.subprocess, 'run', return_value=subprocess.CompletedProcess([], 0, '[41832]', '')) as call:
            self.assertEqual(night.active_training(), [41832])
            self.assertFalse(call.call_args.kwargs.get('shell', False))

    def test_unknown_process_status_refuses_start(self):
        with patch.object(night.subprocess, 'run', return_value=subprocess.CompletedProcess([], 1, '', 'denied')):
            with self.assertRaisesRegex(RuntimeError, 'Cannot verify'):
                night.active_training()

    def test_empty_process_list_is_accepted(self):
        with patch.object(night.subprocess, 'run', return_value=subprocess.CompletedProcess([], 0, json.dumps([]), '')):
            self.assertEqual(night.active_training(), [])


if __name__ == '__main__':
    unittest.main()
