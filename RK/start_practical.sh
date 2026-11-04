#!/usr/bin/env bash
set -euo pipefail
task_here=$(cd -- "$(dirname -- "$0")" && pwd)
task_scene=${1:-intersection}
if [[ $# -gt 0 ]];then shift;fi
exec bash "$task_here/start_dashboard.sh" "$task_scene" --lane-config "$task_here/camera_wide.json" \
  --signal-association practical --lane-overlay sparse "$@"
