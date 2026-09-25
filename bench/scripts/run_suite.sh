#!/usr/bin/env bash
# runs the benchmark workload matrix against one server: warm-up + 5 trials,
# fresh server per trial, CPU/RSS sampling, every command logged to commands.txt
#
# Usage: run_suite.sh --server {gredis|redis} --out <dir> [--dry-run]
#
# gredis must already be built in Release (GREDIS_SERVER_BIN, default
# <repo>/build-release/gredis-server and .../bench_loadgen next to it).
# redis-cli/redis-benchmark/redis-server must be on PATH.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
# shellcheck source=../configs/workloads.sh
source "$REPO_ROOT/bench/configs/workloads.sh"

SERVER=""
OUT=""
DRY_RUN=0

while [ $# -gt 0 ]; do
    case "$1" in
        --server) SERVER="$2"; shift 2 ;;
        --out) OUT="$2"; shift 2 ;;
        --dry-run) DRY_RUN=1; shift ;;
        *) echo "run_suite.sh: unknown argument '$1'" >&2; exit 2 ;;
    esac
done

if [ "$SERVER" != "gredis" ] && [ "$SERVER" != "redis" ]; then
    echo "run_suite.sh: --server must be 'gredis' or 'redis'" >&2
    exit 2
fi
if [ -z "$OUT" ]; then
    echo "run_suite.sh: --out <dir> is required" >&2
    exit 2
fi

mkdir -p "$OUT"
COMMANDS_LOG="$OUT/commands.txt"
: > "$COMMANDS_LOG"
log_cmd() { echo "$*" >> "$COMMANDS_LOG"; }

if [ "$DRY_RUN" = "1" ]; then
    N=1000
    DURATION=1
    TRIALS=1
    WARMUP=0
else
    N=200000
    DURATION=10
    TRIALS=5
    WARMUP=1
fi
KEYSPACE=10000
VALUE_SIZE=3
CLIENTS=50

# pin server and client to separate cores
SERVER_TASKSET=""
CLIENT_TASKSET=""
if command -v taskset >/dev/null 2>&1 && [ "$(nproc)" -ge 8 ]; then
    SERVER_TASKSET="taskset -c 2"
    CLIENT_TASKSET="taskset -c 4-7"
else
    echo "run_suite.sh: WARNING: <8 CPUs or no taskset -- running without core pinning" >&2
fi

SERVER_PID=""
PORT=""

start_server() {
    if [ "$SERVER" = "gredis" ]; then
        PORT=6380
        local bin="${GREDIS_SERVER_BIN:-$REPO_ROOT/build-release/gredis-server}"
        if [ ! -x "$bin" ]; then
            echo "run_suite.sh: gredis-server not found/executable at $bin (build Release first: cmake -B build-release -DCMAKE_BUILD_TYPE=Release && cmake --build build-release)" >&2
            exit 1
        fi
        # shellcheck disable=SC2086
        $SERVER_TASKSET "$bin" --port "$PORT" --log-level warn >"$OUT/server.log" 2>&1 &
        log_cmd "$SERVER_TASKSET $bin --port $PORT --log-level warn &"
    else
        PORT=6379
        if ! command -v redis-server >/dev/null 2>&1; then
            echo "run_suite.sh: redis-server not found on PATH" >&2
            exit 1
        fi
        # shellcheck disable=SC2086
        $SERVER_TASKSET redis-server --port "$PORT" --save "" --appendonly no \
            --io-threads 1 --protected-mode no >"$OUT/server.log" 2>&1 &
        log_cmd "$SERVER_TASKSET redis-server --port $PORT --save '' --appendonly no --io-threads 1 --protected-mode no &"
    fi
    SERVER_PID=$!

    local waited=0
    until redis-cli -p "$PORT" ping >/dev/null 2>&1; do
        sleep 0.1
        waited=$((waited + 1))
        if [ "$waited" -gt 100 ]; then
            echo "run_suite.sh: server did not answer PING within 10s (see $OUT/server.log)" >&2
            kill "$SERVER_PID" 2>/dev/null || true
            exit 1
        fi
    done
}

stop_server() {
    [ -z "$SERVER_PID" ] && return
    kill -TERM "$SERVER_PID" 2>/dev/null || true
    local waited=0
    while kill -0 "$SERVER_PID" 2>/dev/null; do
        sleep 0.1
        waited=$((waited + 1))
        if [ "$waited" -gt 50 ]; then
            kill -KILL "$SERVER_PID" 2>/dev/null || true
            break
        fi
    done
    wait "$SERVER_PID" 2>/dev/null || true
    SERVER_PID=""
}

trap stop_server EXIT

# echoes a space-joined command string since bash can't return an array;
# extra_flags goes before $extra since rb-custom treats everything after
# its own flags as the literal command
build_client_cmd() {
    local kind="$1" extra="$2" c="$3" r="$4" d="$5" extra_flags="${6:-}"
    case "$kind" in
        rb)
            echo redis-benchmark -p "$PORT" -t "$extra" -n "$N" -c "$c" -r "$r" -d "$d" \
                $extra_flags -q
            ;;
        rb-custom)
            echo redis-benchmark -p "$PORT" -n "$N" -c "$c" -r "$r" $extra_flags -q $extra
            ;;
        loadgen)
            local loadgen_bin="${GREDIS_LOADGEN_BIN:-$REPO_ROOT/build-release/bench_loadgen}"
            echo "$loadgen_bin" --port "$PORT" --connections "$c" --keyspace "$r" \
                --value-size "$d" --duration-sec "$DURATION" $extra $extra_flags
            ;;
        *)
            echo "run_suite.sh: unknown workload kind '$kind'" >&2
            exit 1
            ;;
    esac
}

run_setup() {
    local kind="$1" extra="$2"
    [ -z "$kind" ] && return
    local cmd
    cmd="$(build_client_cmd "$kind" "$extra" "$CLIENTS" "$KEYSPACE" "$VALUE_SIZE")"
    log_cmd "(setup) $CLIENT_TASKSET $cmd"
    # shellcheck disable=SC2086
    $CLIENT_TASKSET $cmd >/dev/null 2>&1 || true
}

# One measured trial: fresh server, optional setup, sampled client run.
run_trial() {
    local name="$1" kind="$2" extra="$3" setup_kind="$4" setup_extra="$5"
    local c="$6" r="$7" d="$8" extra_flags="$9" label="${10}" workdir="${11}"

    start_server
    run_setup "$setup_kind" "$setup_extra"

    mkdir -p "$workdir"
    local outfile="$workdir/${label}.txt"
    local proc_csv="$workdir/${label}.proc.csv"

    "$SCRIPT_DIR/sample_proc.sh" "$SERVER_PID" "$((DURATION + 5))" >"$proc_csv" 2>/dev/null &
    local sampler_pid=$!

    local cmd
    cmd="$(build_client_cmd "$kind" "$extra" "$c" "$r" "$d" "$extra_flags")"
    log_cmd "[$name/$label] $CLIENT_TASKSET $cmd"
    set +e
    # shellcheck disable=SC2086
    $CLIENT_TASKSET $cmd >"$outfile" 2>&1
    local rc=$?
    set -e

    kill "$sampler_pid" 2>/dev/null || true
    wait "$sampler_pid" 2>/dev/null || true
    stop_server

    if [ "$rc" -ne 0 ]; then
        echo "run_suite.sh: [$name/$label] client exited with status $rc -- see $outfile" >&2
    fi
}

run_workload() {
    local name="$1" kind="$2" extra="$3" setup_kind="$4" setup_extra="$5"
    local c="$6" r="$7" d="$8" extra_flags="${9:-}"
    local workdir="$OUT/$SERVER/$name"
    echo "==> $name (server=$SERVER c=$c r=$r d=$d)"

    if [ "$WARMUP" = "1" ]; then
        run_trial "$name" "$kind" "$extra" "$setup_kind" "$setup_extra" "$c" "$r" "$d" \
            "$extra_flags" "warmup" "$workdir"
    fi
    for i in $(seq 1 "$TRIALS"); do
        run_trial "$name" "$kind" "$extra" "$setup_kind" "$setup_extra" "$c" "$r" "$d" \
            "$extra_flags" "trial_$i" "$workdir"
    done
}

find_entry() {
    local base="$1"
    for e in "${WORKLOADS[@]}"; do
        if [ "${e%%|*}" = "$base" ]; then
            echo "$e"
            return 0
        fi
    done
    echo "run_suite.sh: no workload named '$base' in bench/configs/workloads.sh" >&2
    exit 1
}

for entry in "${WORKLOADS[@]}"; do
    IFS='|' read -r name kind extra setup_kind setup_extra <<< "$entry"
    run_workload "$name" "$kind" "$extra" "$setup_kind" "$setup_extra" \
        "$CLIENTS" "$KEYSPACE" "$VALUE_SIZE" ""
done

# concurrency sweep: set/get at each -c
for c in "${CONCURRENCY_SWEEP[@]}"; do
    for base in set get; do
        entry="$(find_entry "$base")"
        IFS='|' read -r name kind extra setup_kind setup_extra <<< "$entry"
        run_workload "${name}_c${c}" "$kind" "$extra" "$setup_kind" "$setup_extra" \
            "$c" "$KEYSPACE" "$VALUE_SIZE" ""
    done
done

# pipelining sweep
for p in "${PIPELINE_SWEEP[@]}"; do
    for base in set get; do
        entry="$(find_entry "$base")"
        IFS='|' read -r name kind extra setup_kind setup_extra <<< "$entry"
        if [ "$kind" = "loadgen" ]; then
            flags="--pipeline $p"
        else
            flags="-P $p"
        fi
        run_workload "${name}_p${p}" "$kind" "$extra" "$setup_kind" "$setup_extra" \
            "$CLIENTS" "$KEYSPACE" "$VALUE_SIZE" "$flags"
    done
done

# payload sweep
for d in "${PAYLOAD_SWEEP[@]}"; do
    for base in set get; do
        entry="$(find_entry "$base")"
        IFS='|' read -r name kind extra setup_kind setup_extra <<< "$entry"
        run_workload "${name}_d${d}" "$kind" "$extra" "$setup_kind" "$setup_extra" \
            "$CLIENTS" "$KEYSPACE" "$d" ""
    done
done

echo "run_suite.sh: done. Raw output under $OUT/$SERVER/, commands in $COMMANDS_LOG"
