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
        timeout 1 bash -c "echo >\"/dev/tcp/${host}/${port}\"" >/dev/null 2>&1
    else
        bash -c "echo >\"/dev/tcp/${host}/${port}\"" >/dev/null 2>&1
    fi
}

krpc_wait_for_zookeeper() {
    local host="${KRPC_ZK_HOST:-127.0.0.1}"
    local port="${KRPC_ZK_PORT:-2181}"
    local seconds="${KRPC_ZK_WAIT_SECONDS:-30}"
    local i
    for i in $(seq 1 "${seconds}"); do
        if krpc_tcp_open "${host}" "${port}"; then
            return 0
        fi
        sleep 1
    done
    echo "ZooKeeper at ${host}:${port} was not ready after ${seconds}s." >&2
    return 1
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

krpc_cleanup_runtime() {
    [[ -n "${SERVER1_PID:-}" ]] && kill "${SERVER1_PID}" 2>/dev/null || true
    [[ -n "${SERVER2_PID:-}" ]] && kill "${SERVER2_PID}" 2>/dev/null || true
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
