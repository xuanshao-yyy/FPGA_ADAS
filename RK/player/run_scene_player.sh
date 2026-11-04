#!/usr/bin/env bash
set -euo pipefail
task_here=$(cd -- "$(dirname -- "$0")" && pwd)
task_runtime=${RUNTIME_DIR:-$(cd -- "$task_here/.." && pwd)}
export RUNTIME_DIR="$task_runtime"
for task_arg in "$@";do
 case "$task_arg" in --help|-h|--list|--check|--verify-data|--dry-run) exec python3 "$task_here/scene_player.py" "$@";; esac
done
if [[ ${RK_PREVIEW_SESSION_MANAGED:-0} != 1 ]];then
 if pgrep -x rk_signal_contr >/dev/null;then echo 'An RK test is running. Click EXIT in the old window before starting this player.';exit 2;fi
 export DISPLAY=${DISPLAY:-:0}
 exec python3 "$task_runtime/scripts/desktop_session.py" -- bash "$task_here/run_scene_player.sh" "$@"
fi
exec python3 "$task_here/scene_player.py" "$@"
