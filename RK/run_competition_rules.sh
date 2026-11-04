#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")"
task_scene=${1:-red}; [[ $# == 0 ]] || shift
task_mode=${1:-interactive}; [[ $# == 0 ]] || shift
case "$task_scene" in
  red|green|live) bash run_stable_signal.sh "$task_scene" "$task_mode" --lane-rules --output-root "results/RULES_${task_scene}_${task_mode}" "$@"; exit;;
  lane_change|real_0061|real_0796|real_1077);;
  *) echo 'Usage: bash run_competition_rules.sh red|green|live|lane_change|real_0061|real_0796|real_1077 interactive|evidence|performance';exit 2;;
esac
task_suite=${REPLAY_SUITE:-}
if [[ -z $task_suite ]];then
  for task_try in /userdata/auto_drive/rk3568_replay_suite_20261009 /userdata/auto_drive/competition_replay_20261009;do
    if [[ -f $task_try/data/$task_scene/manifest.json ]];then task_suite=$task_try;break;fi
  done
fi
[[ -n $task_suite ]] || { echo 'Set REPLAY_SUITE to your existing replay suite (road/lane footage is not duplicated in this patch).';exit 2; }
task_config="configs/runtime_${task_scene}.json"
task_metadata=$(python3 scripts/prepare_rule_scene.py --suite "$task_suite" --scene "$task_scene" --output "$task_config")
mapfile -t task_values <<< "$task_metadata"
echo "Scene $task_scene: ${task_values[2]} frames at ${task_values[1]} Hz. CH0 local road, CH1-3 FPGA; adjacent-lane clearance remains unverified."
bash run_stable_signal.sh live "$task_mode" --lane-rules --file0 "${task_values[0]}" --file-fps "${task_values[1]}" --file-max-mib 1024 \
  --signal-config "$task_config" --output-root "results/RULES_${task_scene}_${task_mode}" "$@"
