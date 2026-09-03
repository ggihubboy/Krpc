#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=../scripts/runtime_common.sh
source "$ROOT/scripts/runtime_common.sh"

failed=0
expect() {
    if ! "$@"; then
        echo "FAIL: $*" >&2
        failed=$((failed + 1))
    fi
}

tmp="$(mktemp -d)"
_test_main_pid=$BASHPID
trap '[[ $BASHPID -eq $_test_main_pid ]] && rm -rf "$tmp"' EXIT

PATH="$tmp/empty:$PATH"
mkdir -p "$tmp/empty"
if ( unset KRPC_COMPOSE_BIN; krpc_resolve_compose ); then
    echo "FAIL: resolve_compose should fail without Compose" >&2
    failed=$((failed + 1))
fi

cat >"$tmp/empty/docker-compose" <<'EOF'
#!/usr/bin/env bash
echo "docker-compose $*" >>"${KRPC_COMPOSE_LOG}"
if [[ "$*" == *down* ]]; then
    exit 0
fi
exit 0
EOF
chmod +x "$tmp/empty/docker-compose"
export KRPC_COMPOSE_LOG="$tmp/compose.log"
if ! krpc_resolve_compose || [[ "${COMPOSE_CMD[0]}" != "docker-compose" ]]; then
    echo "FAIL: resolve_compose should use docker-compose fallback" >&2
    failed=$((failed + 1))
fi

export KRPC_ZK_HOST="127.0.0.1"
export KRPC_ZK_PORT="1"
export KRPC_ZK_WAIT_SECONDS="1"
if krpc_wait_for_zookeeper; then
    echo "FAIL: wait_for_zookeeper should time out on a closed port" >&2
    failed=$((failed + 1))
fi

sleep 30 &
SERVER1_PID=$!
sleep 30 &
SERVER2_PID=$!
export KRPC_STARTED_COMPOSE=1
export KRPC_COMPOSE_FILE="$ROOT/docker-compose.yml"
echo "DEBUG compose_cmd=${COMPOSE_CMD[*]} started=${KRPC_STARTED_COMPOSE} file=${KRPC_COMPOSE_FILE} log=${KRPC_COMPOSE_LOG}" >&2
type docker-compose >&2 || true
krpc_cleanup_runtime
echo "DEBUG log_exists=$([[ -f ${KRPC_COMPOSE_LOG} ]] && echo yes || echo no)" >&2
cat "${KRPC_COMPOSE_LOG}" 2>/dev/null >&2 || true
if kill -0 "${SERVER1_PID}" 2>/dev/null || kill -0 "${SERVER2_PID}" 2>/dev/null; then
    echo "FAIL: cleanup_runtime left server processes running" >&2
    failed=$((failed + 1))
    kill "${SERVER1_PID}" "${SERVER2_PID}" 2>/dev/null || true
fi
if [[ ! -s "${KRPC_COMPOSE_LOG}" ]] || ! grep -q down "${KRPC_COMPOSE_LOG}"; then
    echo "FAIL: cleanup_runtime should run compose down" >&2
    failed=$((failed + 1))
fi

meta="$(KRPC_BENCH_METADATA_ONLY=1 KRPC_BUILD_TYPE=Release "$ROOT/scripts/bench.sh")"
for field in date hostname cpu ram kernel compiler build_type command; do
    if ! grep -E "^${field}=" >/dev/null <<<"${meta}"; then
        echo "FAIL: bench metadata missing ${field}" >&2
        echo "${meta}" >&2
        failed=$((failed + 1))
    fi
done

if [[ "${failed}" -ne 0 ]]; then
    echo "${failed} script assertion(s) failed" >&2
    exit 1
fi
echo "script_test ok"
