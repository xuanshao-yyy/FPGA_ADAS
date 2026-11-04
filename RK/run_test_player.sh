#!/usr/bin/env bash
set -euo pipefail
task_here=$(cd -- "$(dirname -- "$0")" && pwd)
if [[ ${DESKTOP_TUNING:-on} != off && ${RK_PREVIEW_SESSION_MANAGED:-0} != 1 ]];then
  export DISPLAY=${DISPLAY:-:0}
  exec python3 "$task_here/scripts/desktop_session.py" -- bash "$task_here/run_test_player.sh" "$@"
fi
task_assets=${ROAD_ASSETS:-"$task_here/../road_checks_20261017_rc2"}
task_scene=${1:-green}; [[ $# == 0 ]] || shift
task_mode=${1:-interactive}; [[ $# == 0 ]] || shift
case "$task_scene" in red|green|neighbor|curve);; *) echo 'Usage: bash run_test_player.sh red|green|neighbor|curve interactive|evidence|performance [runtime options]';exit 2;; esac
case "$task_mode" in interactive|evidence|performance);; *) echo 'Invalid test mode';exit 2;; esac
export GST_GL_PLATFORM=${GST_GL_PLATFORM:-egl}
export GST_GL_API=${GST_GL_API:-gles2}
export GST_GL_WINDOW=${GST_GL_WINDOW:-x11}
echo 'PAUSE freezes the review image and local video clock; FPGA receive continues. Requests are disabled while paused.'
echo 'NEXT finishes current logs, resets rules/tracks, then loads next clip. EXIT stops. SCENE switches signal/lane intent.'
echo 'Interactive runs until EXIT. Performance comparisons must not include pauses; review pauses are logged separately.'
while true;do
  if [[ $task_scene == red ]];then
    task_command=(bash "$task_here/run_competition_rules.sh" red "$task_mode")
  else
    [[ -f "$task_assets/run_road_check.sh" ]] || { echo 'Set ROAD_ASSETS to road_checks_20261017_rc2 (videos are preserved separately).';exit 2; }
    task_command=(bash "$task_assets/run_road_check.sh" "$task_scene" "$task_mode")
  fi
  set +e
  DISPLAY=${DISPLAY:-:0} RUNTIME_DIR="$task_here" "${task_command[@]}" \
    --preview-sink "${PREVIEW_BACKEND:-xv}" --shallow-weight 6 --deep-weight 3 --other-weight 1 --rx-batch-us 1000 --output-root "$task_here/results/PLAYER_${task_scene}_${task_mode}" "$@"
  task_status=$?
  set -e
  [[ $task_status == 20 ]] || exit "$task_status"
  case "$task_scene" in red)task_scene=green;;green)task_scene=neighbor;;neighbor)task_scene=curve;;curve)task_scene=red;;esac
  echo "Loading next video: $task_scene (separate result directory and fresh control state)"
done
