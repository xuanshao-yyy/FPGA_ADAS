#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")"
task_scene=${1:-red}; [[ $# == 0 ]] || shift
task_mode=${1:-interactive}; [[ $# == 0 ]] || shift
bash run_auto_signal.sh "$task_scene" "$task_mode" --signal-stop-hold-ms 1500 --signal-box-hold-ms 500 \
  --output-root "results/STABLE_SIGNAL_${task_scene}_${task_mode}" "$@"
