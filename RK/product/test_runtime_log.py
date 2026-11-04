"""Bounded runtime-log parsing, provenance and retained-run checks."""
from pathlib import Path
import tempfile
import unittest
from runtime_log import tail_rows, collect


class RuntimeLogTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.run = self.root/'results/run'
        self.run.mkdir(parents=True)

    def tearDown(self):
        self.temp.cleanup()

    def test_utf8_quoted_reason(self):
        path = self.run/'user_operations.csv'
        path.write_text('now_s,command,reason\n1,左变道,"实线,禁止"\n', encoding='utf8')
        self.assertEqual(tail_rows(path)[0]['reason'], '实线,禁止')

    def test_budget_and_incomplete_final_record(self):
        path = self.run/'user_operations.csv'
        path.write_text('now_s,command\n'+''.join(f'{i},GO\n' for i in range(10000))+'10000,part', encoding='utf8')
        rows = tail_rows(path, limit=4, budget=100)
        self.assertEqual([r['now_s'] for r in rows], ['9996','9997','9998','9999'])

    def test_order_and_filter_raw_samples(self):
        (self.run/'user_operations.csv').write_text('now_s,command\n3,左变道\n', encoding='utf8')
        (self.run/'lane_rules.csv').write_text('now_s,event,rule\n4,MODEL_OBSERVATION,X\n2,LANE_STATE_CHANGE,ALARM_SOLID_REQUEST\n', encoding='utf8')
        data, run = collect(self.root, None, self.run)
        self.assertEqual([r['monotonic_s'] for r in data['rows']], [4.,3.,2.])
        self.assertEqual(run, self.run.resolve())

    def test_outside_previous_run_rejected(self):
        data, run = collect(self.root, None, self.root.parent)
        self.assertEqual(data['rows'], [])
        self.assertIsNone(run)

    def test_empty_or_absent_process(self):
        data, run = collect(self.root, 2147483647)
        self.assertEqual(data['rows'], [])
        self.assertIsNone(run)

    def test_invalid_timestamp_ignored(self):
        (self.run/'user_operations.csv').write_text('now_s,command\nbad,GO\n5,STOP\n', encoding='utf8')
        data, _ = collect(self.root, None, self.run)
        self.assertEqual(len(data['rows']), 1)


if __name__ == '__main__':
    unittest.main()
