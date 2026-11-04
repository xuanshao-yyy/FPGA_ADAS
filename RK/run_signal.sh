#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")"
task_scene=${1:-red}; [[ $# == 0 ]] || shift
task_mode=${1:-interactive}; [[ $# == 0 ]] || shift
task_args=()
case "$task_scene" in
  red) task_config=configs/red_arrow.json; task_file=data/red_arrow.rgb565;;
  green) task_config=configs/green_straight.json; task_file=data/green_straight.rgb565;;
  live) task_config=configs/live_manual.json;task_file=;;
  *) echo 'Usage: bash run_signal.sh red|green|live interactive|performance|evidence [runtime options]';exit 2;;
esac
case "$task_mode" in
  interactive) task_seconds=0; task_save=0;;
  performance) task_seconds=60; task_save=0;;
  evidence) task_seconds=12; task_save=1;;
  *) echo 'Unknown mode';exit 2;;
esac
if [[ -n $task_file ]];then
  python3 scripts/check_media.py "$task_scene"
  task_args+=(--file0 "$task_file" --file-fps 30 --file-loop on)
fi
[[ $(uname -m) == aarch64 ]] || { echo 'Run on RK3568';exit 1; }
[[ -n ${DISPLAY:-} ]] || { echo 'Run from RK desktop terminal with DISPLAY set';exit 1; }
chmod u+x rk_signal_control
export LD_LIBRARY_PATH="$PWD/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
# Only delivery-verified BEST detector/lane weights; no alternate compact/hybrid model.
./rk_signal_control --model models/traffic_yolo11n_640x640_i8.rknn --front-model models/traffic_yolo11n_640x384_i8.rknn \
  --lane-model models/small4_pad1_i8.rknn --ai-mode mosaic --front-channel 0 \
  --channels 0,1,2,3 --ai-channels 0,1,2,3 --ai-fps 0 --lane-fps 0 \
  --shallow-weight 4 --deep-weight 3 --other-weight 2 --tensor-output native --lane-post async \
  --lane-hybrid off --lane-deadline-ms 500 --light-candidates \
  --signal-config "$task_config" --seconds "$task_seconds" --save-every "$task_save" \
  --output-root "results/SIGNAL_${task_scene}_${task_mode}" "${task_args[@]}" "$@"
