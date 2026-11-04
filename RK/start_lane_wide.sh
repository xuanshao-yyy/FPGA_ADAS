#!/usr/bin/env bash
set -euo pipefail
task_here=$(cd -- "$(dirname -- "$0")" && pwd)
# Wider extraction area for the observed screen-filmed road case; old config retained.
exec bash "$task_here/start_dashboard.sh" "$@" --lane-config "$task_here/camera_wide.json"
