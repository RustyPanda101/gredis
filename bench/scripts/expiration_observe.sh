#!/usr/bin/env bash
# sustained SET ... PX 50 stream via bench_loadgen, observing RSS and
# DBSIZE over time. Not part of run_suite.sh's matrix. Needs an
# already-running, already-empty gredis-server.
#
# Usage: expiration_observe.sh <port> <server_pid> <duration_sec> <out_dir>
set -euo pipefail

port="${1:?usage: expiration_observe.sh <port> <server_pid> <duration_sec> <out_dir>}"
server_pid="${2:?}"
duration="${3:?}"
outdir="${4:?}"
mkdir -p "$outdir"

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/../.." && pwd)"

echo "elapsed_sec,dbsize" > "$outdir/dbsize.csv"
start="$(date +%s.%N)"
(
    while true; do
        now="$(date +%s.%N)"
        elapsed="$(awk -v a="$now" -v b="$start" 'BEGIN{printf "%.3f", a-b}')"
        size="$(redis-cli -p "$port" dbsize 2>/dev/null | tr -dc '0-9')"
        echo "${elapsed},${size:-0}" >> "$outdir/dbsize.csv"
        sleep 0.5
    done
) &
dbsize_pid=$!
trap 'kill "$dbsize_pid" 2>/dev/null || true' EXIT

"$script_dir/sample_proc.sh" "$server_pid" "$duration" > "$outdir/proc.csv" &
proc_pid=$!

loadgen_bin="${GREDIS_LOADGEN_BIN:-$repo_root/build-release/bench_loadgen}"
"$loadgen_bin" --port "$port" --connections 50 --keyspace 2000000 --get-pct 0 \
    --expire-ms 50 --duration-sec "$duration" > "$outdir/loadgen.txt"

# keep sampling a bit past load's end so DBSIZE's convergence back to 0 is
# visible in the tail of dbsize.csv, not just the sustained-load portion
sleep 5
kill "$dbsize_pid" "$proc_pid" 2>/dev/null || true
wait "$dbsize_pid" "$proc_pid" 2>/dev/null || true

echo "expiration_observe.sh: done. See $outdir/{dbsize.csv,proc.csv,loadgen.txt}"
