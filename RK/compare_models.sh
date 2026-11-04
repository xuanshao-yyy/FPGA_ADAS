#!/usr/bin/env bash
set -euo pipefail
task_here=$(cd -- "$(dirname -- "$0")" && pwd)
exec python3 "$task_here/scripts/compare_models.py" "$@"
