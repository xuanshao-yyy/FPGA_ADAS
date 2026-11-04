#!/usr/bin/env bash
set -euo pipefail
task_here=$(cd -- "$(dirname -- "$0")" && pwd)
cd "$task_here"
exec python3 product/lifecycle.py stop "$@"
