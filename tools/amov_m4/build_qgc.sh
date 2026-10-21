#!/usr/bin/env bash
set -euo pipefail

task_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
task_repo=$(cd -- "$task_dir/../.." && pwd)
task_qt_root=${AMOV_QT_ROOT:-${HOME}/.local/amov-m4/Qt}
task_build=${AMOV_QGC_BUILD:-build-amov-m4}
case "$task_build" in
    /*) task_build_abs=$task_build ;;
    *) task_build_abs=$task_repo/$task_build ;;
esac
task_python=${AMOV_QGC_PYTHON:-/opt/qgc-python/bin/python}
if [[ -z ${AMOV_QGC_PYTHON:-} && -f "$task_build_abs/venv-tools/pyvenv.cfg" ]]; then
    task_python=$task_build_abs/venv-tools/bin/python
fi

bash "$task_dir/container.sh" cmake -S . -B "$task_build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug "-DCMAKE_PREFIX_PATH=$task_qt_root/lib/cmake" \
    -DQGC_BUILD_TESTING=ON -DQGC_UOM_BENCH_TEST=ON -DQGC_ENABLE_GST_VIDEOSTREAMING=OFF \
    "-DPython3_EXECUTABLE=$task_python" "-DPython_EXECUTABLE=$task_python" \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON "-DCPM_SOURCE_CACHE=$task_repo/.cache/CPM-amov-m4" "$@"
bash "$task_dir/container.sh" cmake --build "$task_build" --parallel "${AMOV_BUILD_JOBS:-2}"
