#!/usr/bin/env bash
set -euo pipefail

readonly MUDUO_VERSION="v2.0.2"
readonly INSTALL_PREFIX="${1:-${HOME}/.local}"
readonly WORK_ROOT="${KRPC_DEPS_DIR:-${TMPDIR:-/tmp}/krpc-deps}"
readonly SOURCE_DIR="${WORK_ROOT}/muduo-${MUDUO_VERSION}"
readonly BUILD_DIR="${WORK_ROOT}/muduo-build-${MUDUO_VERSION}"

if [[ -f "${INSTALL_PREFIX}/lib/libmuduo_net.a" &&
      -f "${INSTALL_PREFIX}/lib/libmuduo_base.a" &&
      -f "${INSTALL_PREFIX}/include/muduo/net/TcpServer.h" ]]; then
    echo "Muduo ${MUDUO_VERSION} is already installed in ${INSTALL_PREFIX}"
    exit 0
fi

mkdir -p "${WORK_ROOT}" "${INSTALL_PREFIX}"

if [[ ! -d "${SOURCE_DIR}/.git" ]]; then
    git clone --depth 1 --branch "${MUDUO_VERSION}" \
        https://github.com/chenshuo/muduo.git "${SOURCE_DIR}"
fi

cmake -S "${SOURCE_DIR}" -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${INSTALL_PREFIX}" \
    -DMUDUO_BUILD_EXAMPLES=OFF
cmake --build "${BUILD_DIR}" --parallel "${KRPC_BUILD_JOBS:-2}"
cmake --install "${BUILD_DIR}"

echo "Muduo ${MUDUO_VERSION} installed in ${INSTALL_PREFIX}"
