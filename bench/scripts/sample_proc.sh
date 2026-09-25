#!/usr/bin/env bash
# samples a process's CPU% and RSS every `interval` seconds, prints CSV
# rows "elapsed_sec,cpu_pct,rss_kb" until the PID disappears or duration_sec elapses
#
# Usage: sample_proc.sh <pid> [duration_sec] [interval_sec]
#   duration_sec: 0 (default) = sample until the process exits
#   interval_sec: default 0.5
set -euo pipefail

pid="${1:?usage: sample_proc.sh <pid> [duration_sec] [interval_sec]}"
duration="${2:-0}"
interval="${3:-0.5}"

clk_tck="$(getconf CLK_TCK)"
start="$(date +%s.%N)"
prev_ticks=""
prev_time=""

echo "elapsed_sec,cpu_pct,rss_kb"

while [ -d "/proc/$pid" ]; do
    now="$(date +%s.%N)"
    elapsed="$(awk -v a="$now" -v b="$start" 'BEGIN{printf "%.3f", a-b}')"

    if [ "$duration" != "0" ]; then
        past="$(awk -v e="$elapsed" -v d="$duration" 'BEGIN{print (e>=d)?1:0}')"
        [ "$past" = "1" ] && break
    fi

    stat_line="$(cat "/proc/$pid/stat" 2>/dev/null)" || break
    # cut at the LAST ") " since comm itself can contain spaces/parens;
    # remainder starts at field 3, so utime/stime are fields 12/13 of it
    rest="${stat_line##*) }"
    utime="$(echo "$rest" | awk '{print $12}')"
    stime="$(echo "$rest" | awk '{print $13}')"
    ticks=$((utime + stime))

    rss_kb="$(awk '/^VmRSS:/{print $2}' "/proc/$pid/status" 2>/dev/null || echo 0)"
    [ -z "$rss_kb" ] && rss_kb=0

    if [ -n "$prev_ticks" ]; then
        tick_delta=$((ticks - prev_ticks))
        time_delta="$(awk -v a="$now" -v b="$prev_time" 'BEGIN{printf "%.6f", a-b}')"
        cpu_pct="$(awk -v td="$tick_delta" -v dt="$time_delta" -v c="$clk_tck" \
            'BEGIN{ if (dt>0) printf "%.1f", (td/c)/dt*100; else print "0.0" }')"
    else
        cpu_pct="0.0"
    fi

    echo "${elapsed},${cpu_pct},${rss_kb}"

    prev_ticks=$ticks
    prev_time=$now
    sleep "$interval"
done
