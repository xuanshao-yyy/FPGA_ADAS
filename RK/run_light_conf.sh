#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")"
task_conf=${1:-0.15}; [[ $# == 0 ]] || shift
task_scene=${1:-red}; [[ $# == 0 ]] || shift
task_mode=${1:-interactive}; [[ $# == 0 ]] || shift
if [[ $task_conf == legacy ]]; then
  bash run_signal.sh "$task_scene" "$task_mode" --output-root "results/LIGHT_CONF_legacy_${task_scene}_${task_mode}" "$@"
else
  bash run_signal.sh "$task_scene" "$task_mode" --light-conf "$task_conf" --output-root "results/LIGHT_CONF_${task_conf}_${task_scene}_${task_mode}" "$@"
fi
