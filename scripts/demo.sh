#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
# shellcheck source=runtime_common.sh
source "$ROOT/scripts/runtime_common.sh"

BIN="${KRPC_BIN_DIR:-$ROOT/build/bin}"

if [[ ! -x "$BIN/server" || ! -x "$BIN/client" ]]; then
    echo "Build the project first: cmake --build build -j2" >&2
    exit 1
fi

if ! command -v docker >/dev/null 2>&1; then
    echo "docker is required to start ZooKeeper for this demo." >&2
    exit 1
fi

TMP="$(mktemp -d)"
CONF="$TMP/test.conf"
CONF2="$TMP/test-8001.conf"
export KRPC_ZK_HOST_PORT="${KRPC_ZK_HOST_PORT:-12181}"
export KRPC_ZK_PORT="$KRPC_ZK_HOST_PORT"
export KRPC_COMPOSE_FILE="$ROOT/docker-compose.yml"
export COMPOSE_PROJECT_NAME="${KRPC_COMPOSE_PROJECT:-krpc-demo}"

sed "s/^zookeeperport=.*/zookeeperport=${KRPC_ZK_HOST_PORT}/" "$ROOT/bin/test.conf" >"$CONF"
sed "s/^zookeeperport=.*/zookeeperport=${KRPC_ZK_HOST_PORT}/" "$ROOT/bin/test-8001.conf" >"$CONF2"

krpc_resolve_compose
export KRPC_STARTED_COMPOSE=1
cleanup() {
    krpc_cleanup_runtime
    rm -rf "$TMP"
}
trap cleanup EXIT

"${COMPOSE_CMD[@]}" -f "$KRPC_COMPOSE_FILE" up -d
krpc_wait_for_zookeeper
krpc_wait_for_zookeeper_cli

"$BIN/server" -i "$CONF" &
SERVER1_PID=$!
"$BIN/server" -i "$CONF2" &
SERVER2_PID=$!
krpc_wait_for_process "$SERVER1_PID" 5
krpc_wait_for_process "$SERVER2_PID" 5
krpc_wait_for_tcp 127.0.0.1 8000 15 "RPC server"
krpc_wait_for_tcp 127.0.0.1 8001 15 "RPC server"

"$BIN/client" -i "$CONF"
echo "Demo finished: Login + 32KB EchoBlob succeeded against a two-instance cluster."
