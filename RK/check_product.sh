#!/usr/bin/env bash
set -euo pipefail
task_here=$(cd -- "$(dirname -- "$0")" && pwd)
cd "$task_here"
task_failed=0
for task_name in rk_signal_control rk_park_stream;do
  if [[ ! -x "$task_name" ]];then echo "MISSING executable: $task_name";task_failed=1;fi
done
for task_plugin in appsrc queue mpph264enc h264parse mp4mux filesink;do
  if gst-inspect-1.0 "$task_plugin" >/dev/null 2>&1;then echo "PASS plugin $task_plugin";else echo "MISSING GStreamer plugin: $task_plugin";task_failed=1;fi
done
python3 -m compileall -q product
ip -br addr
df -h /userdata
if [[ $task_failed -ne 0 ]];then echo 'Preflight failed; preserve logs. No software encoding fallback.';exit 1;fi
echo 'Preflight only. FPGA bit, QC01 applied ACK, QV03 input and MPP encoding still require integration acceptance.'
