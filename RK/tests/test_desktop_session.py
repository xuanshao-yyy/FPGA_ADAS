"""Linux-only recovery tests with a file-backed fake compositor, no X11/NPU."""
from pathlib import Path
import contextlib
import fcntl
import importlib.util
import io
import json
import os
import signal
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

SOURCE=Path(__file__).resolve().parents[1]/'scripts/desktop_session.py'
spec=importlib.util.spec_from_file_location('desktop_session',SOURCE)
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)


class RecoveryTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix='desktop_recovery_test_')
        self.root=Path(self.temp.name);(self.root/'scripts').mkdir();(self.root/'results').mkdir()
        self.setting=self.root/'setting';self.setting.write_text('true')
        self.helper=self.root/'fake_compositor.py'
        self.helper.write_text("from pathlib import Path\nimport sys\np=Path(sys.argv[1])\nif len(sys.argv)==4:\n p.write_text(sys.argv[3]);p.with_suffix('.log').open('a').write(sys.argv[3]+'\\n')\nelse:print(p.read_text())\n")
        self.cmd=[sys.executable,str(self.helper),str(self.setting)]
        self.state=self.root/'results/desktop_restore.json'
        self.lock=self.root/'session.lock'
        self.stack=contextlib.ExitStack()
        self.stack.enter_context(patch.object(module,'__file__',str(self.root/'scripts/desktop_session.py')))
        self.stack.enter_context(patch.object(module,'desktop',return_value=(os.getuid(),self.cmd)))
        self.stack.enter_context(patch.object(module,'active_previews',return_value=[]))
        self.stack.enter_context(patch.object(module,'lock_path',return_value=self.lock))
        self.stack.enter_context(patch.object(module,'boot_id',return_value='boot-A'))
        self.stack.enter_context(patch.dict(os.environ,{'DISPLAY':':0'}))
        self.stack.enter_context(contextlib.redirect_stdout(io.StringIO()))

    def tearDown(self):
        self.stack.close();self.temp.cleanup()

    def pending(self,original='true',boot='boot-A'):
        record={'original':original,'restored':False,'display':':0'}
        if boot is not None:record['boot_id']=boot
        self.state.write_text(json.dumps(record));return record

    def run_session(self,command=None,args=None):
        command=command or [sys.executable,'-c',"import os;assert os.environ['RK_PREVIEW_SESSION_MANAGED']=='1'"]
        with patch.object(sys,'argv',['desktop_session.py']+(args if args is not None else ['--']+command)):
            return module.main()

    def writes(self):
        path=self.setting.with_suffix('.log')
        return path.read_text().splitlines() if path.exists() else []

    def test_normal_exit_restores_true_and_records_boot(self):
        self.assertEqual(self.run_session(),0)
        self.assertEqual(self.writes(),['false','true'])
        state=json.loads(self.state.read_text())
        self.assertTrue(state['restored']);self.assertEqual(state['boot_id'],'boot-A')

    def test_original_false_is_preserved(self):
        self.setting.write_text('false');self.run_session()
        self.assertEqual(self.writes(),['false','false'])
        self.assertEqual(json.loads(self.state.read_text())['original'],'false')

    def test_legacy_stale_marker_no_longer_blocks(self):
        self.pending(original='false',boot=None);self.setting.write_text('false')
        self.assertEqual(self.run_session(),0)
        self.assertTrue(json.loads(self.state.read_text())['restored'])
        self.assertEqual(len(list((self.root/'results/desktop_sessions').glob('*recovered_abandoned_session.json'))),1)

    def test_same_boot_restores_before_next_launch(self):
        self.pending();self.setting.write_text('false');self.run_session()
        self.assertEqual(self.writes(),['true','false','true'])

    def test_old_boot_does_not_override_current_desktop(self):
        self.pending(boot='boot-OLD');self.setting.write_text('false');self.run_session()
        self.assertEqual(self.writes(),['false','false'])
        self.assertEqual(len(list((self.root/'results/desktop_sessions').glob('*new_boot_use_current.json'))),1)

    def test_active_preview_blocks_both_start_and_manual_restore(self):
        old=self.pending()
        with patch.object(module,'active_previews',return_value=['123']):
            for args in [None,['--restore']]:
                with self.assertRaisesRegex(RuntimeError,'still running'):self.run_session(args=args)
        self.assertEqual(self.writes(),[]);self.assertEqual(json.loads(self.state.read_text()),old)

    def test_global_lock_blocks_overlapping_wrapper(self):
        with self.lock.open('w') as held:
            fcntl.flock(held,fcntl.LOCK_EX|fcntl.LOCK_NB)
            with self.assertRaisesRegex(RuntimeError,'Another preview session'):self.run_session()
        self.assertEqual(self.writes(),[])

    def test_spawn_failure_still_restores(self):
        with self.assertRaises(FileNotFoundError):self.run_session(['/this/command/does/not/exist'])
        self.assertEqual(self.setting.read_text(),'true')
        self.assertTrue(json.loads(self.state.read_text())['restored'])

    def test_child_failure_still_restores_and_preserves_exit_code(self):
        self.assertEqual(self.run_session([sys.executable,'-c','raise SystemExit(7)']),7)
        self.assertTrue(json.loads(self.state.read_text())['restored'])

    def test_sigterm_is_forwarded_and_restores(self):
        command=[sys.executable,'-c','import os,signal,time;os.kill(os.getppid(),signal.SIGTERM);time.sleep(10)']
        self.assertEqual(self.run_session(command),128+signal.SIGTERM)
        self.assertTrue(json.loads(self.state.read_text())['restored'])

    def test_cleanup_timeout_cannot_skip_desktop_restore(self):
        command=['fake-stuck-preview']
        class Child:
            pid=99999999
            def poll(self):return None
            def wait(self,timeout=None):
                if timeout is None:raise RuntimeError('fake initial wait failure')
                if timeout==12:raise subprocess.TimeoutExpired(command,timeout)
                return 0
        original=module.subprocess.Popen
        def spawn(args,**kwargs):return Child() if args==command else original(args,**kwargs)
        with patch.object(module.subprocess,'Popen',side_effect=spawn),patch.object(module.os,'killpg') as kill:
            with self.assertRaisesRegex(RuntimeError,'fake initial wait failure'):self.run_session(command)
            self.assertEqual([c.args[1] for c in kill.call_args_list],[signal.SIGTERM,signal.SIGKILL])
        self.assertTrue(json.loads(self.state.read_text())['restored'])

    def test_bad_record_or_wrong_desktop_is_not_silently_discarded(self):
        for record in [{'original':'invalid','restored':False},
                       {'original':'true','restored':False,'display':':8'}]:
            self.state.write_text(json.dumps(record))
            with self.assertRaises(RuntimeError):self.run_session()
            self.assertEqual(json.loads(self.state.read_text()),record)
        self.assertEqual(self.writes(),[])

    def test_restore_failure_remains_explicitly_pending(self):
        configure=module.configure
        def fail_restore(cmd,value):
            if value=='true':raise RuntimeError('fake restore failure')
            return configure(cmd,value)
        with patch.object(module,'configure',side_effect=fail_restore):
            with self.assertRaisesRegex(RuntimeError,'fake restore failure'):self.run_session()
        state=json.loads(self.state.read_text())
        self.assertFalse(state['restored']);self.assertIn('fake restore failure',state['restore_error'])

    def test_manual_restore_is_idempotent_without_state(self):
        self.assertEqual(self.run_session(args=['--restore']),0)
        self.assertEqual(self.writes(),[])


if __name__=='__main__':unittest.main(verbosity=2)
