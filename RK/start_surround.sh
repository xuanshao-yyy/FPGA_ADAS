#!/usr/bin/env bash
set -euo pipefail
task_here=$(cd -- "$(dirname -- "$0")" && pwd)
if [[ ${1:-live} == live ]]; then
  exec python3 "$task_here/scripts/start_live.py" "$@"
fi
exec bash "$task_here/start_surround_history.sh" "$@"
