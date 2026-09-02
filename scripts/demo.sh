#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

BIN="${KRPC_BIN_DIR:-$ROOT/build/bin}"
CONF="$ROOT/bin/test.conf"
CONF2="$ROOT/bin/test-8001.conf"

if [[ ! -x "$BIN/server" || ! -x "$BIN/client" ]]; then
    echo "Build the project first: cmake --build build -j2" >&2
    exit 1
fi

if ! command -v docker >/dev/null 2>&1; then
    echo "docker is required to start ZooKeeper for this demo." >&2
    exit 1
fi

docker compose -f "$ROOT/docker-compose.yml" up -d
for _ in $(seq 1 30); do
    if (echo ruok | nc -w 1 127.0.0.1 2181 2>/dev/null | grep -q imok) || \
       nc -z 127.0.0.1 2181 2>/dev/null; then
        break
    fi
    sleep 1
done

cleanup() {
    [[ -n "${SERVER1_PID:-}" ]] && kill "$SERVER1_PID" 2>/dev/null || true
    [[ -n "${SERVER2_PID:-}" ]] && kill "$SERVER2_PID" 2>/dev/null || true
    wait || true
}
trap cleanup EXIT

"$BIN/server" -i "$CONF" &
SERVER1_PID=$!
"$BIN/server" -i "$CONF2" &
SERVER2_PID=$!
sleep 2

"$BIN/client" -i "$CONF"
echo "Demo finished: Login + 32KB EchoBlob succeeded against a two-instance cluster."
