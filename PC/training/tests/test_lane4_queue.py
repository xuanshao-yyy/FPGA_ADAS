"""Queue order, failure handling and predecessor completion; no GPU training."""
from contextlib import redirect_stdout
import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'code'))
import train_lane4_queue as queue


class Lane4QueueTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        (self.root/'runs').mkdir()
        self.data = self.root/'data'
        self.data.mkdir()
        for name in ['dataset.json', 'train.jsonl', 'val.jsonl']:
            (self.data/name).write_text('{}', encoding='utf-8')

    def completed(self, output, variant='small'):
        (output/'weights').mkdir(parents=True)
        settings = dict(dataset=str(self.data), stage='smoke', variant=variant)
        for name, key in [('dataset.json', 'dataset_config_sha256'), ('train.jsonl', 'train_index_sha256'),
                          ('val.jsonl', 'val_index_sha256')]:
            settings[key] = hashlib.sha256((self.data/name).read_bytes()).hexdigest()
        (output/'args.json').write_text(json.dumps(settings), encoding='utf-8')
        (output/'report.json').write_text(json.dumps(dict(stage='smoke', variant=variant,
            completed_epochs=1, checkpoint_reload_passed=True, best_foreground_miou=.1)), encoding='utf-8')
        for name in ['best.pt', 'last.pt']:
            (output/'weights'/name).write_bytes(b'fixture: metadata tests only')

    def invoke(self, executor, extra=None):
        arguments = ['queue', '--stage', 'smoke', '--data', str(self.data), *(extra or [])]
        with patch.object(queue, 'PROJECT_ROOT', self.root), patch.object(sys, 'argv', arguments), \
                patch.object(queue, 'execute_training', side_effect=executor), redirect_stdout(io.StringIO()):
            queue.main()

    def test_serial_order_and_distinct_checkpoints(self):
        seen = []
        def execute(command, status, _):
            variant = command[command.index('--variant')+1]
            self.assertEqual(command[0], sys.executable)
            seen.append(variant)
            self.completed(Path(command[command.index('--output')+1]), variant)
        self.invoke(execute)
        self.assertEqual(seen, ['small', 'nano'])
        status = json.loads(next((self.root/'runs').glob('lane4_queue_*/queue.json')).read_text())
        self.assertEqual(status['state'], 'completed')
        self.assertNotEqual(status['jobs'][0]['output'], status['jobs'][1]['output'])
        self.assertFalse((self.root/'runs/.lane4_queue.lock').exists())

    def test_failure_does_not_launch_second_model(self):
        seen = []
        def execute(command, *_):
            seen.append(command[command.index('--variant')+1])
            raise RuntimeError('fixture failure')
        with self.assertRaisesRegex(RuntimeError, 'fixture failure'):
            self.invoke(execute)
        self.assertEqual(seen, ['small'])
        status = json.loads(next((self.root/'runs').glob('lane4_queue_*/queue.json')).read_text())
        self.assertEqual(status['state'], 'failed')
        self.assertEqual(status['jobs'][0]['state'], 'failed')
        self.assertFalse((self.root/'runs/.lane4_queue.lock').exists())

    def test_existing_interrupted_run_does_not_start_nano(self):
        previous = self.root/'runs/previous'
        self.completed(previous)
        (previous/'report.json').unlink()
        with patch.object(queue, 'wait_for_process') as wait:
            with self.assertRaises(FileNotFoundError):
                self.invoke(lambda *_: self.fail('Must not start after interrupted training'),
                            ['--after-run', str(previous), '--wait-pid', '123'])
            wait.assert_called_once_with(123)

    def test_successful_predecessor_starts_only_nano(self):
        previous = self.root/'runs/previous'
        self.completed(previous)
        seen = []
        def execute(command, *_):
            variant = command[command.index('--variant')+1]
            seen.append(variant)
            self.completed(Path(command[command.index('--output')+1]), variant)
        with patch.object(queue, 'wait_for_process') as wait:
            self.invoke(execute, ['--after-run', str(previous), '--wait-pid', '123'])
            wait.assert_called_once_with(123)
        self.assertEqual(seen, ['nano'])

    def test_changed_data_and_wrong_variant_are_rejected(self):
        previous = self.root/'runs/previous'
        self.completed(previous)
        with self.assertRaisesRegex(RuntimeError, 'wrong model variant'):
            queue.require_completed(previous, self.data, 'smoke', 'nano')
        (self.data/'train.jsonl').write_text('changed', encoding='utf-8')
        with self.assertRaisesRegex(RuntimeError, 'Dataset changed'):
            queue.require_completed(previous, self.data, 'smoke')

    def test_existing_queue_lock_rejects_duplicate(self):
        lock = self.root/'runs/.lane4_queue.lock'
        lock.write_text('fixture existing queue', encoding='utf-8')
        with self.assertRaises(FileExistsError):
            self.invoke(lambda *_: self.fail('Must not launch duplicate queue'))
        self.assertEqual(lock.read_text(), 'fixture existing queue')

    @unittest.skipUnless(os.name == 'nt', 'Windows process handle')
    def test_waits_for_owned_process_exit(self):
        child = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(.2)'])
        try:
            queue.wait_for_process(child.pid)
            self.assertEqual(child.wait(timeout=2), 0)
        finally:
            if child.poll() is None:
                child.terminate()
                child.wait()


if __name__ == '__main__':
    unittest.main()
