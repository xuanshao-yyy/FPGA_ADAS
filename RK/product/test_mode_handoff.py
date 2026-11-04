from pathlib import Path
import argparse,json,os,tempfile,unittest
from unittest.mock import patch
from server import Supervisor
class ModeHandoffTests(unittest.TestCase):
 def setUp(self):
  self.temp=tempfile.TemporaryDirectory();self.root=Path(self.temp.name)
  self.app=Supervisor(argparse.Namespace(state=self.root,cloud_url='',scene='lanes',web_only=True,bind_a='127.0.0.1',peer_a='127.0.0.1',drive_args=[]))
  self.app.update(mode='DRIVE');self.calls=[]
  self.app.start_mode_hub=lambda:self.calls.append('cover')
  self.app.stop_mode_hub=lambda:self.calls.append('uncover')
  self.app.set_fpga=lambda mode:self.calls.append('ack_'+mode)
  self.app.stop_process=lambda child:self.calls.append('stop')
  self.app.stop_encoder=lambda:None;self.app.stop_panorama=lambda:None
  self.app.start_drive=lambda:self.calls.append('drive')
  self.app.wait_drive_ready=lambda:self.calls.append('first_render')
 def tearDown(self):self.app.events.db.close();self.app.evidence.close();self.temp.cleanup()
 def test_park_keeps_return_page_after_video_ready(self):
  self.app.encoder=argparse.Namespace(poll=lambda:None);self.app.reader=argparse.Namespace(error='')
  self.app.start_encoder=lambda:self.calls.append('encoder');self.app.fragments.info=lambda:{'ready':True}
  self.app.transition('PARK');self.assertEqual(self.app.status['mode'],'PARK');self.assertNotIn('uncover',self.calls)
  state=json.loads((self.root/'display_mode.json').read_text());self.assertEqual(state['mode'],'PARK');self.assertEqual(state['target'],'PARK')
 def test_drive_uncover_only_after_first_render(self):
  self.app.update(mode='PARK');self.app.transition('DRIVE')
  self.assertEqual(self.calls,['cover','ack_DRIVE','drive','first_render','uncover']);self.assertEqual(self.app.status['phase'],'驾驶画面已就绪')
 def test_panorama_waits_full_qv04_before_uncover(self):
  self.app.panorama=argparse.Namespace(poll=lambda:None);self.app.start_panorama=lambda:self.calls.append('panorama')
  self.app.panorama_state=lambda:{'ready':True};self.app.transition('PANORAMA')
  self.assertEqual(self.app.status['mode'],'PANORAMA');self.assertLess(self.calls.index('ack_PANORAMA'),self.calls.index('uncover'))
 def test_failed_drive_never_removes_cover(self):
  self.app.update(mode='PARK');self.app.wait_drive_ready=lambda:(_ for _ in ()).throw(RuntimeError('no new frame'))
  self.app.transition('DRIVE');self.assertEqual(self.app.status['mode'],'FAULT');self.assertNotIn('uncover',self.calls);self.assertIn('no new frame',self.app.status['error'])
 def test_busy_mode_rejects_repeated_request(self):
  self.app.update(mode='SWITCHING')
  with self.assertRaises(ValueError):self.app.command('PARK')
  self.assertTrue(self.app.commands.empty())
 def test_stale_render_marker_does_not_finish_handoff(self):
  self.app.args.web_only=False;self.app.drive=argparse.Namespace(poll=lambda:None);self.app.drive_started=10
  class Stop:
   stopped=False
   def is_set(self):return self.stopped
   def wait(self,seconds):self.stopped=True
  self.app.stop=Stop()
  with patch('server.read_json',return_value={'pid':os.getpid(),'monotonic_s':9}):
   with self.assertRaisesRegex(RuntimeError,'新画面'):Supervisor.wait_drive_ready(self.app)
if __name__=='__main__':unittest.main()
