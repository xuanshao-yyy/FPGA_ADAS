#!/usr/bin/env bash
set -euo pipefail
task_here=$(cd -- "$(dirname -- "$0")" && pwd)
export DISPLAY=${DISPLAY:-:0}
export SCENE_PLAYER_DIR="$task_here/full_player"
export SCENE_ASSET_DIR=${FULL_VIDEO_ASSET_DIR:-/userdata/test_auto_drive/full_video_20261020}
task_scene=${1:-full_01}
if [[ $# -gt 0 ]];then shift;fi
exec bash "$task_here/compare_models.sh" A "$task_scene" --output-root "$task_here/results/full_video" --file-max-mib 2048 "$@"
