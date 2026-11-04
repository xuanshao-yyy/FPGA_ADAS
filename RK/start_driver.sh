#!/usr/bin/env bash
set -euo pipefail
task_here=$(cd -- "$(dirname -- "$0")" && pwd)
task_scene=${1:-red_green}
[[ $# == 0 ]] || shift
export DISPLAY=${DISPLAY:-:0}
exec bash "$task_here/compare_models.sh" A "$task_scene" "$@"
