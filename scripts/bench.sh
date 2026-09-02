#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${KRPC_BIN_DIR:-$ROOT/build/bin}"
CONF="$ROOT/bin/test.conf"

if [[ ! -x "$BIN/krpc_bench" ]]; then
    echo "Build the project first: cmake --build build -j2" >&2
    exit 1
fi

export KRPC_BENCH_THREADS="${KRPC_BENCH_THREADS:-$(nproc)}"
export KRPC_BENCH_REQUESTS="${KRPC_BENCH_REQUESTS:-10000}"
export KRPC_BENCH_PAYLOAD="${KRPC_BENCH_PAYLOAD:-0}"

echo "uname=$(uname -a)"
echo "nproc=$(nproc)"
echo "build_dir=$BIN"
"$BIN/krpc_bench" -i "$CONF"
