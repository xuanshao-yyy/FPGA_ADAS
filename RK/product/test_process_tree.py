import os,signal,subprocess,sys,time,unittest
from process_tree import descendants,identity,wait_exited
class ProcessTreeTests(unittest.TestCase):
 def test_launcher_exit_waits_detached_owned_child(self):
  script="import subprocess,sys,time; p=subprocess.Popen([sys.executable,'-c','import time; time.sleep(.7)'],start_new_session=True); print(p.pid,flush=True); time.sleep(10)"
  parent=subprocess.Popen([sys.executable,'-c',script],stdout=subprocess.PIPE,text=True,start_new_session=True)
  child=int(parent.stdout.readline());owned=descendants(parent.pid);self.assertIn(child,owned)
  os.killpg(parent.pid,signal.SIGTERM);parent.wait();start=time.monotonic();wait_exited(owned,3)
  self.assertGreater(time.monotonic()-start,.2);self.assertIsNone(identity(child));parent.stdout.close()
 def test_pid_reuse_does_not_wait_unrelated_process(self):
  wait_exited({os.getpid():'invalid-old-start-time'},0)
 def test_running_owned_process_fails_explicitly(self):
  stamp=identity(os.getpid())[1]
  with self.assertRaises(RuntimeError):wait_exited({os.getpid():stamp},0)
if __name__=='__main__':unittest.main()
