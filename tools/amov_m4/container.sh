#!/usr/bin/env bash
set -euo pipefail

task_repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
task_qt_root=${AMOV_QT_ROOT:-${HOME}/.local/amov-m4/Qt}
task_state=${AMOV_QGC_STATE:-${HOME}/.local/share/amov-m4-qgc}
task_image=${AMOV_M4_IMAGE:-amov-m4-qgc:qt612-gcc12}

if [[ ! -x "$task_qt_root/bin/qmake" ]]; then
    echo "Qt kit is missing: set AMOV_QT_ROOT to the Qt 6.12 kit directory." >&2
    exit 1
fi
if [[ $# -eq 0 ]]; then
    echo "Usage: bash tools/amov_m4/container.sh COMMAND [ARG ...]" >&2
    exit 2
fi
mkdir -p "$task_state/config" "$task_state/data" "$task_state/cache" "$task_state/runtime"
chmod 700 "$task_state/runtime"
task_args=(--rm --network host --user "$(id -u):$(id -g)"
    --volume "$task_repo:$task_repo" --workdir "$task_repo"
    --volume "$task_qt_root:$task_qt_root:ro" --volume "$task_state:/qgc-state"
    --env "LD_LIBRARY_PATH=$task_qt_root:$task_qt_root/lib"
    --env "QT_ROOT_DIR=$task_qt_root" --env "LANG=C.UTF-8"
    --env "XDG_CONFIG_HOME=/qgc-state/config" --env "XDG_DATA_HOME=/qgc-state/data"
    --env "XDG_CACHE_HOME=/qgc-state/cache" --env "XDG_RUNTIME_DIR=/qgc-state/runtime"
    --env "CCACHE_DIR=/qgc-state/cache/ccache"
    --env "NPM_CONFIG_CACHE=/qgc-state/cache/npm"
    --env HTTP_PROXY --env HTTPS_PROXY --env NO_PROXY
    --env "QT_QPA_PLATFORM=${QT_QPA_PLATFORM:-xcb}"
    --env QT_QUICK_BACKEND --env QSG_RHI_BACKEND --env LIBGL_ALWAYS_SOFTWARE --env QT_LOGGING_RULES
    --env QGC_UOM_MOCK_URL)

if [[ -n ${DISPLAY:-} ]]; then
    task_args+=(--env DISPLAY --volume /tmp/.X11-unix:/tmp/.X11-unix:ro)
    task_authority=${XAUTHORITY:-${HOME}/.Xauthority}
    if [[ -f "$task_authority" ]]; then
        task_args+=(--volume "$task_authority:/qgc-xauthority:ro" --env "XAUTHORITY=/qgc-xauthority")
    fi
fi
if [[ -n ${QGC_SERIAL_DEVICE:-} ]]; then
    if [[ ! -c "$QGC_SERIAL_DEVICE" ]]; then
        echo "QGC_SERIAL_DEVICE must name an existing serial device." >&2
        exit 1
    fi
    task_args+=(--device "$QGC_SERIAL_DEVICE:$QGC_SERIAL_DEVICE"
        --group-add "$(stat -Lc '%g' "$QGC_SERIAL_DEVICE")")
fi
exec docker run "${task_args[@]}" "$task_image" "$@"
