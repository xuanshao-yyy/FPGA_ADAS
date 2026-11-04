#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
task_ui=${TASK_UI_RUNTIME:-/home/alientek/rk3568_signal_mouse_20261016/host_ui/runtime}
"$task_ui/usr/bin/Xvfb" :176 -screen 0 1600x1000x24 -nolisten tcp -ac -fp "$task_ui/usr/share/fonts/X11/misc" > verification/automatic_xvfb.log 2>&1 &
task_server=$!
trap 'kill "$task_server" 2>/dev/null || true' EXIT
export DISPLAY=:176
for task_try in {1..30}; do
  if xdpyinfo >/dev/null 2>&1; then break; fi
  sleep .1
done
task_sysroot=${RK_SYSROOT:-/home/alientek/rk3568_linux_sdk/debian/binary}
task_qemu=${RK_QEMU:-/home/alientek/rk3568_lane_suite_20261005/host_tools/usr/bin/qemu-aarch64}
LD_LIBRARY_PATH="$PWD/lib:$task_sysroot/usr/lib/aarch64-linux-gnu" "$task_qemu" -L "$task_sysroot" \
  ./build-signal/test_toolbar_x11 verification/automatic_toolbar.ppm
LD_LIBRARY_PATH="$PWD/lib:$task_sysroot/usr/lib/aarch64-linux-gnu" "$task_qemu" -L "$task_sysroot" \
  ./build-signal/test_dashboard_x11 verification/top_dashboard_fixture.ppm
