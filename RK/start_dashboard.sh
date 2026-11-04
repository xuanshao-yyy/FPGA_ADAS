#!/usr/bin/env bash
set -euo pipefail
task_here=$(cd -- "$(dirname -- "$0")" && pwd)
export DISPLAY=${DISPLAY:-:0}
export SCENE_PLAYER_DIR="$task_here/all_player"
export SCENE_ASSET_DIR="$task_here/video/replay"
task_scene=${1:-intersection}
if [[ $# -gt 0 ]];then shift;fi
exec bash "$task_here/compare_models.sh" A "$task_scene" --output-root "$task_here/results/top_dashboard" --file-max-mib 2048 "$@"
