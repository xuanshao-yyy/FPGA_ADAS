#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")"
task_scene=${1:-red}; [[ $# == 0 ]] || shift
task_mode=${1:-interactive}; [[ $# == 0 ]] || shift
case "$task_scene" in red|green|live);; *) echo 'Usage: bash run_auto_signal.sh red|green|live interactive|evidence|performance';exit 2;; esac
bash run_signal.sh "$task_scene" "$task_mode" --light-conf 0.15 --signal-mode auto --light-rgb-model models/light_rgb_128_i8_kl256.rknn \
  --signal-config "configs/auto_${task_scene}.json" \
  --output-root "results/AUTO_SIGNAL_${task_scene}_${task_mode}" "$@"
