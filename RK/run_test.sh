#!/usr/bin/env bash
set -euo pipefail
task_here=$(cd -- "$(dirname -- "$0")" && pwd)
export RUNTIME_DIR="$task_here"
if [[ -z ${SCENE_ASSET_DIR:-} ]];then
  for task_assets in "$task_here/player" /userdata/test_auto_drive/scene_player_20261018 /userdata/test_auto_drive/rk3568_scene_player_20261018;do
    if [[ -d "$task_assets/data" ]];then export SCENE_ASSET_DIR="$task_assets";break;fi
  done
fi
exec bash "${SCENE_PLAYER_DIR:-$task_here/player}/run_scene_player.sh" "$@"
