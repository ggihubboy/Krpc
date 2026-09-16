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
cat >"$tmp/empty/docker" <<'EOF'
#!/usr/bin/env bash
exit 1
EOF
chmod +x "$tmp/empty/docker"
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

cat >"$tmp/graceful-server" <<'EOF'
#!/usr/bin/env bash
trap 'sleep 0.2; echo "server1-stopped" >>"${KRPC_COMPOSE_LOG}"; exit 0' TERM
touch "${KRPC_SERVER_READY}"
while true; do
    sleep 1
done
EOF
chmod +x "$tmp/graceful-server"
export KRPC_SERVER_READY="$tmp/server.ready"
"$tmp/graceful-server" &
SERVER1_PID=$!
while [[ ! -f "$KRPC_SERVER_READY" ]]; do sleep 0.01; done
sleep 30 &
SERVER2_PID=$!
export KRPC_STARTED_COMPOSE=1
export KRPC_COMPOSE_FILE="$ROOT/docker-compose.yml"
krpc_cleanup_runtime
if kill -0 "${SERVER1_PID}" 2>/dev/null || kill -0 "${SERVER2_PID}" 2>/dev/null; then
    echo "FAIL: cleanup_runtime left server processes running" >&2
    failed=$((failed + 1))
    kill "${SERVER1_PID}" "${SERVER2_PID}" 2>/dev/null || true
fi
if [[ ! -s "${KRPC_COMPOSE_LOG}" ]] || ! grep -q down "${KRPC_COMPOSE_LOG}"; then
    echo "FAIL: cleanup_runtime should run compose down" >&2
    failed=$((failed + 1))
fi
if [[ "$(sed -n '1p' "${KRPC_COMPOSE_LOG}")" != "server1-stopped" ]]; then
    echo "FAIL: cleanup_runtime should wait for servers before compose down" >&2
    failed=$((failed + 1))
fi

if ! command -v protoc >/dev/null 2>&1; then
    echo "FAIL: protoc must be on PATH so CI and local builds can generate protobuf C++" >&2
    failed=$((failed + 1))
else
    mkdir -p "$tmp/proto"
    if ! protoc --cpp_out="$tmp/proto" -I "$ROOT/src" "$ROOT/src/Krpcheader.proto"; then
        echo "FAIL: protoc cannot generate Krpcheader.proto" >&2
        failed=$((failed + 1))
    elif [[ ! -f "$tmp/proto/Krpcheader.pb.cc" || ! -f "$tmp/proto/Krpcheader.pb.h" ]]; then
        echo "FAIL: protoc did not write Krpcheader.pb.cc/.h" >&2
        failed=$((failed + 1))
    fi
    if ! protoc --cpp_out="$tmp/proto" -I "$ROOT/example" "$ROOT/example/user.proto"; then
        echo "FAIL: protoc cannot generate user.proto" >&2
        failed=$((failed + 1))
    elif [[ ! -f "$tmp/proto/user.pb.cc" || ! -f "$tmp/proto/user.pb.h" ]]; then
        echo "FAIL: protoc did not write user.pb.cc/.h" >&2
        failed=$((failed + 1))
    fi
fi

for stale in \
    "$ROOT/src/Krpcheader.pb.cc" \
    "$ROOT/src/Krpcheader.pb.h" \
    "$ROOT/src/include/Krpcheader.pb.h" \
    "$ROOT/example/user.pb.cc" \
    "$ROOT/example/user.pb.h"; do
    if [[ -e "$stale" ]]; then
        echo "FAIL: stale generated protobuf file must not live in the source tree: $stale" >&2
        failed=$((failed + 1))
    fi
done

if ! grep -E '^[[:space:]]*find_package\(Protobuf REQUIRED\)' "$ROOT/CMakeLists.txt" >/dev/null; then
    echo "FAIL: root CMakeLists.txt must find Protobuf" >&2
    failed=$((failed + 1))
elif ! awk '
    /protobuf_MODULE_COMPATIBLE/ { compatible = 1 }
    /find_package\(Protobuf REQUIRED\)/ { found = 1; if (!compatible) missing = 1 }
    END { exit(missing || !found) }
' "$ROOT/CMakeLists.txt"; then
    echo "FAIL: protobuf_MODULE_COMPATIBLE must be set before find_package(Protobuf) so Ubuntu 24.04 fills Protobuf_PROTOC_EXECUTABLE" >&2
    failed=$((failed + 1))
fi

if ! grep -E 'protobuf_generate(_cpp)?' "$ROOT/cmake/KrpcProtobuf.cmake" >/dev/null; then
    echo "FAIL: cmake/KrpcProtobuf.cmake must use official protobuf_generate/protobuf_generate_cpp" >&2
    failed=$((failed + 1))
fi

if grep -n 'get_target_property' "$ROOT/cmake/KrpcProtobuf.cmake" >/dev/null; then
    if ! grep -E 'EXISTS "\$\{_loc\}"' "$ROOT/cmake/KrpcProtobuf.cmake" >/dev/null; then
        echo "FAIL: imported protoc location must be checked with EXISTS so CMake -NOTFOUND is not used as the compiler" >&2
        failed=$((failed + 1))
    fi
fi

# Ubuntu 24.04 ships ZooKeeper 3.9. Sync C APIs such as zoo_wget_children
# and zoo_get are inside #ifdef THREADED. Include them only through ZkCApi.h.
zk_api="$ROOT/src/include/ZkCApi.h"
if [[ ! -f "$zk_api" ]]; then
    echo "FAIL: missing $zk_api; it must #define THREADED before zookeeper.h" >&2
    failed=$((failed + 1))
elif ! awk '
    /#define[[:space:]]+THREADED/ { defined = 1 }
    /#include[[:space:]]*[<"]zookeeper\/zookeeper.h[>"]/ {
        inc = 1
        if (!defined) bad = 1
    }
    END { exit(!(inc && defined) || bad) }
' "$zk_api"; then
    echo "FAIL: ZkCApi.h must #define THREADED before including zookeeper.h" >&2
    failed=$((failed + 1))
fi

while IFS= read -r f; do
    echo "FAIL: $f includes zookeeper.h directly; include ZkCApi.h instead so ZooKeeper 3.9 exposes zoo_wget_children" >&2
    failed=$((failed + 1))
done < <(
    grep -RIl --include='*.h' --include='*.cc' 'zookeeper/zookeeper.h' \
        "$ROOT/src" "$ROOT/tests" 2>/dev/null \
        | grep -v '/ZkCApi.h$' || true
)

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
