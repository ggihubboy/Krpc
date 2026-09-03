#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=runtime_common.sh
source "$ROOT/scripts/runtime_common.sh"

BIN="${KRPC_BIN_DIR:-$ROOT/build/bin}"
CONF="$ROOT/bin/test.conf"
CACHE="${KRPC_CMAKE_CACHE:-$ROOT/build/CMakeCache.txt}"

export KRPC_BENCH_THREADS="${KRPC_BENCH_THREADS:-$(nproc)}"
export KRPC_BENCH_REQUESTS="${KRPC_BENCH_REQUESTS:-10000}"
export KRPC_BENCH_PAYLOAD="${KRPC_BENCH_PAYLOAD:-0}"

command="KRPC_BENCH_THREADS=${KRPC_BENCH_THREADS} KRPC_BENCH_REQUESTS=${KRPC_BENCH_REQUESTS} KRPC_BENCH_PAYLOAD=${KRPC_BENCH_PAYLOAD} ./scripts/bench.sh"
krpc_print_bench_metadata "$BIN" "$command" "$CACHE"

if [[ "${KRPC_BENCH_METADATA_ONLY:-0}" == "1" ]]; then
    exit 0
fi

if [[ ! -x "$BIN/krpc_bench" ]]; then
    echo "Build the project first: cmake --build build -j2" >&2
    exit 1
fi

"$BIN/krpc_bench" -i "$CONF"
