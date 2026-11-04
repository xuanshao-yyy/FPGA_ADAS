#!/usr/bin/env bash
set -euo pipefail
task_here=$(cd -- "$(dirname -- "$0")" && pwd)
cd "$task_here"
export DISPLAY=${DISPLAY:-:0}
export PREVIEW_BACKEND=rk
export V8_MODEL_DIR="$task_here/models"
exec bash start_product.sh --scene live --park-fps 30 --park-bitrate 5000000 -- --preview-fps 30 --preview-sink rk "$@"
