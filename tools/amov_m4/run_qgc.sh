#!/usr/bin/env bash
set -euo pipefail

task_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
task_repo=$(cd -- "$task_dir/../.." && pwd)
task_binary=${AMOV_QGC_BINARY:-${task_repo}/build-amov-m4/Debug/QGroundControl}
if [[ ! -x "$task_binary" ]]; then
    echo "QGroundControl has not been built: $task_binary" >&2
    exit 1
fi
export QGC_UOM_MOCK_URL=${QGC_UOM_MOCK_URL:-http://127.0.0.1:8765}
exec bash "$task_dir/container.sh" "$task_binary" "$@"
