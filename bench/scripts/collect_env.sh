#!/usr/bin/env bash
# dumps machine/kernel/CPU/governor/compiler/commit/comparison-server
# version so results are reproducible later
#
# Usage: collect_env.sh > env.txt
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

echo "Date: $(date -u '+%Y-%m-%d %H:%M:%S UTC')"
echo

echo "=== uname ==="
uname -a
echo

echo "=== CPU ==="
if command -v lscpu >/dev/null 2>&1; then
    lscpu | grep -E '^(Model name|CPU\(s\)|Thread\(s\) per core|Core\(s\) per socket|Socket\(s\))' || lscpu
else
    echo "lscpu not found"
fi
echo

echo "=== Memory ==="
free -h
echo

echo "=== CPU governor ==="
found_governor=0
for f in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    if [ -e "$f" ]; then
        echo "$f: $(cat "$f")"
        found_governor=1
    fi
done
[ "$found_governor" = "0" ] && echo "(no cpufreq scaling_governor files found)"
echo

echo "=== Compiler ==="
"${CXX:-g++}" --version | head -1
echo

echo "=== gredis ==="
commit="$(git -C "$REPO_ROOT" rev-parse HEAD 2>/dev/null || echo unknown)"
echo "commit: $commit"
dirty="$(git -C "$REPO_ROOT" status --porcelain 2>/dev/null || true)"
if [ -n "$dirty" ]; then
    echo "WARNING: working tree is DIRTY -- results not reproducible from the commit alone"
    echo "$dirty"
fi
echo

echo "=== Comparison server ==="
if command -v redis-server >/dev/null 2>&1; then
    redis-server --version
else
    echo "redis-server: not found on PATH"
fi
if command -v redis-benchmark >/dev/null 2>&1; then
    redis-benchmark --version
else
    echo "redis-benchmark: not found on PATH"
fi
echo "Note: on this machine /usr/bin/redis-server IS Valkey -- label results"
echo "'Valkey <version>', never 'Redis', unless real Redis is installed separately."
