#!/usr/bin/env bash

set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
project_root="$(cd -- "${script_dir}/.." && pwd)"
qgc_binary="${QGC_BINARY:-${project_root}/build/Debug/QGroundControl}"

if [[ ! -x "${qgc_binary}" ]]; then
    printf 'QGroundControl executable not found: %s\n' "${qgc_binary}" >&2
    printf 'Set QGC_BINARY to the build you want to launch.\n' >&2
    exit 1
fi

qt_root="${QGC_QT_ROOT:-}"
if [[ -z "${qt_root}" ]]; then
    qt_core_path="$(ldd "${qgc_binary}" 2>/dev/null | awk '$1 == "libQt6Core.so.6" {print $3; exit}')"
    if [[ -n "${qt_core_path}" && -f "${qt_core_path}" ]]; then
        qt_root="$(dirname -- "$(dirname -- "${qt_core_path}")")"
    fi
fi

if [[ -z "${qt_root}" || ! -d "${qt_root}/lib" ]]; then
    printf 'Qt installation not found. Set QGC_QT_ROOT to the Qt installation directory.\n' >&2
    exit 1
fi

openssl_root="${QGC_OPENSSL_ROOT:-}"
if [[ -z "${openssl_root}" ]]; then
    for candidate in "${CONDA_PREFIX:-}" "${HOME}/miniconda3" "${HOME}/anaconda3" /opt/conda; do
        if [[ -f "${candidate}/lib/libssl.so.3" && -f "${candidate}/lib/libcrypto.so.3" ]]; then
            openssl_root="${candidate}"
            break
        fi
    done
fi

if [[ -z "${openssl_root}" || ! -f "${openssl_root}/lib/libssl.so.3" || ! -f "${openssl_root}/lib/libcrypto.so.3" ]]; then
    printf 'OpenSSL 3 runtime not found. Set QGC_OPENSSL_ROOT to its installation directory.\n' >&2
    exit 1
fi

# Load only the OpenSSL libraries from the Conda environment. Adding the whole
# Conda lib directory to LD_LIBRARY_PATH can override the system OpenGL/Mesa
# libraries used by Qt Quick.
export LD_PRELOAD="${openssl_root}/lib/libcrypto.so.3:${openssl_root}/lib/libssl.so.3${LD_PRELOAD:+:${LD_PRELOAD}}"
export LD_LIBRARY_PATH="${qt_root}:${qt_root}/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"

if [[ -d "${qt_root}/plugins" ]]; then
    export QT_PLUGIN_PATH="${QT_PLUGIN_PATH:-${qt_root}/plugins}"
fi

exec "${qgc_binary}" "$@"
