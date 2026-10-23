#!/usr/bin/env bash
set -euo pipefail

qgc_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
qgc_binary="$qgc_root/build-rid-management/Debug/QGroundControl"
qgc_qt_root=${QGC_RID_QT_ROOT:-/home/amovlgf/.local/amov-m4/Qt}
qgc_image=${QGC_RID_IMAGE:-amov-m4-qgc:qt612-gcc12}
qgc_smoke=false
if [[ ${1:-} == --headless-smoke ]]; then
    qgc_smoke=true
    shift
fi
if [[ ! -x $qgc_binary ]]; then
    printf 'QGC executable not found: %s\n' "$qgc_binary" >&2
    exit 1
fi
if [[ ! -d $qgc_qt_root ]]; then
    printf 'Qt runtime not found: %s\n' "$qgc_qt_root" >&2
    exit 1
fi
if "$qgc_smoke"; then
    qgc_state=$(mktemp -d "${TMPDIR:-/tmp}/qgc-rid-smoke.XXXXXXXX")
    trap 'rm -rf -- "$qgc_state"' EXIT
else
    qgc_state=${QGC_RID_STATE_DIR:-${XDG_DATA_HOME:-$HOME/.local/share}/QGroundControlRID}
    mkdir -p -- "$qgc_state"
fi
qgc_args=(run --rm --user "$(id -u):$(id -g)"
    --mount "type=bind,src=$qgc_root,dst=$qgc_root,readonly"
    --mount "type=bind,src=$qgc_qt_root,dst=$qgc_qt_root,readonly"
    --mount "type=bind,src=$qgc_state,dst=/qgc-state"
    --workdir "$qgc_root"
    --env "LD_LIBRARY_PATH=$qgc_qt_root:$qgc_qt_root/lib"
    --env "LANG=${LANG:-C.UTF-8}"
    --env "XDG_CONFIG_HOME=/qgc-state/config"
    --env "XDG_CACHE_HOME=/qgc-state/cache"
    --env "XDG_DATA_HOME=/qgc-state/data"
    --env "QT_QUICK_BACKEND=software"
    --env "LIBGL_ALWAYS_SOFTWARE=1")
if "$qgc_smoke"; then
    qgc_args+=(--network none --env "QT_QPA_PLATFORM=offscreen")
    qgc_app_args=(--simple-boot-test --allow-multiple "$@")
else
    if [[ -z ${DISPLAY:-} ]]; then
        printf 'Start this script from the desktop session (DISPLAY is unset).\n' >&2
        exit 1
    fi
    qgc_args+=(--network host --env "DISPLAY=$DISPLAY" --env "QT_QPA_PLATFORM=xcb"
        --env "QT_X11_NO_MITSHM=1"
        --mount "type=bind,src=/tmp/.X11-unix,dst=/tmp/.X11-unix,readonly")
    qgc_xauthority=${XAUTHORITY:-$HOME/.Xauthority}
    if [[ -f $qgc_xauthority ]]; then
        qgc_args+=(--mount "type=bind,src=$qgc_xauthority,dst=/qgc-xauthority,readonly"
            --env "XAUTHORITY=/qgc-xauthority")
    fi
    shopt -s nullglob
    for qgc_serial_device in /dev/ttyACM* /dev/ttyUSB*; do
        qgc_args+=(--device "$qgc_serial_device:$qgc_serial_device"
            --group-add "$(stat -c %g "$qgc_serial_device")")
    done
    qgc_app_args=("$@")
fi
docker "${qgc_args[@]}" "$qgc_image" "$qgc_binary" "${qgc_app_args[@]}"
