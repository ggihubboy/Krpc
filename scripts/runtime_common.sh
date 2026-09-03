#!/usr/bin/env bash
# Shared helpers for demo, bench metadata, and integration tests.
# Safe to source. Override commands through KRPC_* environment variables.

krpc_resolve_compose() {
    COMPOSE_CMD=()
    if [[ -n "${KRPC_COMPOSE_BIN:-}" ]]; then
        # shellcheck disable=SC2206
        COMPOSE_CMD=(${KRPC_COMPOSE_BIN})
        return 0
    fi
    if command -v docker >/dev/null 2>&1 && docker compose version >/dev/null 2>&1; then
        COMPOSE_CMD=(docker compose)
        return 0
    fi
    if command -v docker-compose >/dev/null 2>&1; then
        COMPOSE_CMD=(docker-compose)
        return 0
    fi
    echo "Docker Compose V2 (docker compose) or docker-compose is required." >&2
    return 1
}

krpc_tcp_open() {
    local host="$1"
    local port="$2"
    if command -v timeout >/dev/null 2>&1; then
        timeout 1 bash -c "exec 3<>\"/dev/tcp/${host}/${port}\"" >/dev/null 2>&1
    else
        bash -c "exec 3<>\"/dev/tcp/${host}/${port}\"" >/dev/null 2>&1
    fi
}

krpc_wait_for_tcp() {
    local host="$1"
    local port="$2"
    local seconds="${3:-30}"
    local label="${4:-service}"
    local i
    for i in $(seq 1 "${seconds}"); do
        if krpc_tcp_open "${host}" "${port}"; then
            return 0
        fi
        sleep 1
    done
    echo "${label} at ${host}:${port} was not ready after ${seconds}s." >&2
    return 1
}

krpc_wait_for_zookeeper() {
    local host="${KRPC_ZK_HOST:-127.0.0.1}"
    local port="${KRPC_ZK_PORT:-2181}"
    local seconds="${KRPC_ZK_WAIT_SECONDS:-30}"
    krpc_wait_for_tcp "${host}" "${port}" "${seconds}" "ZooKeeper"
}

krpc_wait_for_process() {
    local pid="$1"
    local seconds="${2:-5}"
    local i
    for i in $(seq 1 "${seconds}"); do
        if kill -0 "${pid}" 2>/dev/null; then
            return 0
        fi
        sleep 1
    done
    echo "Process ${pid} exited before it became ready." >&2
    return 1
}

krpc_compose_down() {
    local compose_file="${1:-}"
    if [[ ${#COMPOSE_CMD[@]} -eq 0 || -z "${compose_file}" ]]; then
        return 0
    fi
    "${COMPOSE_CMD[@]}" -f "${compose_file}" down --remove-orphans || true
}

krpc_stop_process() {
    local pid="${1:-}"
    local seconds="${2:-${KRPC_STOP_WAIT_SECONDS:-10}}"
    local ticks=$((seconds * 10))
    local i state
    [[ -z "${pid}" ]] && return 0
    if ! kill -0 "${pid}" 2>/dev/null; then
        wait "${pid}" 2>/dev/null || true
        return 0
    fi

    kill "${pid}" 2>/dev/null || true
    for ((i = 0; i < ticks; ++i)); do
        if ! kill -0 "${pid}" 2>/dev/null; then
            wait "${pid}" 2>/dev/null || true
            return 0
        fi
        state="$(ps -o stat= -p "${pid}" 2>/dev/null || true)"
        if [[ -z "${state}" || "${state}" == Z* ]]; then
            wait "${pid}" 2>/dev/null || true
            return 0
        fi
        sleep 0.1
    done

    echo "Process ${pid} did not stop after ${seconds}s; sending SIGKILL." >&2
    kill -KILL "${pid}" 2>/dev/null || true
    wait "${pid}" 2>/dev/null || true
}

krpc_cleanup_runtime() {
    krpc_stop_process "${SERVER1_PID:-}"
    krpc_stop_process "${SERVER2_PID:-}"
    if [[ "${KRPC_STARTED_COMPOSE:-0}" == "1" ]]; then
        krpc_compose_down "${KRPC_COMPOSE_FILE:-}"
    fi
}

krpc_print_bench_metadata() {
    local bin_dir="${1:-}"
    local command="${2:-}"
    local cache="${3:-}"
    local compiler
    compiler="$(${CXX:-c++} --version 2>/dev/null | head -n 1)"
    local build_type="${KRPC_BUILD_TYPE:-}"
    if [[ -z "${build_type}" && -f "${cache}" ]]; then
        build_type="$(sed -n 's/^CMAKE_BUILD_TYPE:STRING=//p' "${cache}" | head -n 1)"
    fi
    local ram_kb
    ram_kb="$(awk '/MemTotal:/ {print $2}' /proc/meminfo 2>/dev/null || true)"
    echo "date=$(date -Iseconds)"
    echo "hostname=$(hostname)"
    echo "cpu=$(nproc) cores"
    echo "ram=${ram_kb:-unknown} kB"
    echo "kernel=$(uname -srm)"
    echo "compiler=${compiler:-unknown}"
    echo "build_type=${build_type:-unknown}"
    echo "command=${command}"
    echo "build_dir=${bin_dir}"
}
