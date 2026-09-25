# Workload matrix definitions read by bench/scripts/run_suite.sh
#
# Each WORKLOADS entry is "name|kind|extra|setup_kind|setup_extra",
# pipe-separated because `extra` can contain literal colons (e.g.
# "h:__rand_int__"), which rules out ':' as a delimiter.
# `setup_kind`/`setup_extra` (may both be empty) run once against a fresh
# server before the measured command, to pre-populate data read-only
# workloads need (otherwise GETs just hit nil, a different workload).
#
# `kind` is one of:
#   rb         -- `redis-benchmark -t <extra>`, using the run's $N/$C/$R/$D
#   rb-custom  -- `redis-benchmark` with `extra` appended as a literal
#                 command (redis-benchmark substitutes __rand_int__ itself)
#   loadgen    -- `bench_loadgen` with `extra` as additional flags, using
#                 the run's $C/$R/$D(value size)/$DURATION
WORKLOADS=(
    "set|rb|set||"
    "get|rb|get|rb|set"
    "incr|rb|incr||"
    "mixed_90_10|loadgen|--get-pct 90|rb|set"
    "mixed_50_50|loadgen|--get-pct 50|rb|set"
    "hash_hset|rb-custom|hset h:__rand_int__ f v||"
    "hash_hget|rb-custom|hget h:__rand_int__ f|rb-custom|hset h:__rand_int__ f v"
    "zset_zadd|rb-custom|zadd zs __rand_int__ m:__rand_int__||"
    "zset_zrank|rb-custom|zrank zs m:__rand_int__|rb-custom|zadd zs __rand_int__ m:__rand_int__"
    "zset_zrange|rb-custom|zrange zs 0 10|rb-custom|zadd zs __rand_int__ m:__rand_int__"
    "zset_zpopmin|rb-custom|zpopmin zs|rb-custom|zadd zs __rand_int__ m:__rand_int__"
    "expiration|loadgen|--get-pct 0 --expire-ms 50||"
)

# re-runs "set" and "get" at each -c
CONCURRENCY_SWEEP=(1 10 50 200 1000)

# -P for redis-benchmark, --pipeline for loadgen
PIPELINE_SWEEP=(1 16 64)

# Payload sweep: -d for redis-benchmark, --value-size for loadgen.
PAYLOAD_SWEEP=(16 256 4096)
