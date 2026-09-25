cmake_minimum_required(VERSION 3.20)
project(gredis CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

# asan is the default dev/test build, tsan gets its own build dir
# (can't combine both), release/profiling builds use none
set(GREDIS_SANITIZE "none" CACHE STRING "Sanitizer set: none, asan, tsan, fuzzer")
set_property(CACHE GREDIS_SANITIZE PROPERTY STRINGS none asan tsan fuzzer)

if(GREDIS_SANITIZE STREQUAL "asan")
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
    add_link_options(-fsanitize=address,undefined)
elseif(GREDIS_SANITIZE STREQUAL "tsan")
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
elseif(GREDIS_SANITIZE STREQUAL "fuzzer")
    # clang only. fuzzer-no-link gives every target the coverage
    # instrumentation without pulling in libFuzzer's main() -- only
    # fuzz_gredis below links the real -fsanitize=fuzzer, else it'd
    # collide with gredis-server's own main()
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        message(FATAL_ERROR "GREDIS_SANITIZE=fuzzer requires clang++ (got '${CMAKE_CXX_COMPILER_ID}') -- "
                             "configure with -DCMAKE_CXX_COMPILER=clang++")
    endif()
    add_compile_options(-fsanitize=fuzzer-no-link,address,undefined -fno-omit-frame-pointer
                         -fno-sanitize-recover=undefined)
    add_link_options(-fsanitize=address,undefined)
elseif(NOT GREDIS_SANITIZE STREQUAL "none")
    message(FATAL_ERROR "GREDIS_SANITIZE must be one of: none, asan, tsan, fuzzer (got '${GREDIS_SANITIZE}')")
endif()

add_compile_options(
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion -Werror
)

# everything but main.cpp goes in here, so server + every test link the same code
add_library(gredis_core STATIC
    src/config.cpp
    src/util/log.cpp
    src/util/clock.cpp
    src/util/fd.cpp
    src/util/parse.cpp
    src/util/thread_pool.cpp
    src/util/crc32.cpp
    src/net/socket_util.cpp
    src/net/event_loop.cpp
    src/net/timer_queue.cpp
    src/net/buffer.cpp
    src/net/server.cpp
    src/protocol/resp_parser.cpp
    src/protocol/resp_writer.cpp
    src/storage/value.cpp
    src/storage/database.cpp
    src/commands/dispatcher.cpp
    src/commands/cmd_server.cpp
    src/commands/cmd_string.cpp
    src/commands/cmd_keys.cpp
    src/commands/cmd_hash.cpp
    src/commands/cmd_list.cpp
    src/commands/cmd_set.cpp
    src/commands/cmd_zset.cpp
    src/commands/cmd_persist.cpp
    src/persistence/snapshot.cpp
)
target_include_directories(gredis_core PUBLIC src)

find_package(Threads REQUIRED)
target_link_libraries(gredis_core PUBLIC Threads::Threads)

add_executable(gredis-server src/main.cpp)
target_link_libraries(gredis-server PRIVATE gredis_core)

enable_testing()

function(gredis_unit_test name)
    add_executable(${name} tests/unit/${name}.cpp)
    target_link_libraries(${name} PRIVATE gredis_core)
    target_include_directories(${name} PRIVATE tests/unit)
    add_test(NAME ${name} COMMAND ${name})
    # so a hung join (thread pool dtor deadlock etc) fails just this
    # test instead of hanging all of ctest
    set_tests_properties(${name} PROPERTIES TIMEOUT 120)
endfunction()

gredis_unit_test(test_config)
gredis_unit_test(test_fd)
gredis_unit_test(test_socket_util)
gredis_unit_test(test_event_loop)
gredis_unit_test(test_buffer)
gredis_unit_test(test_resp_parser)
gredis_unit_test(test_resp_writer)
gredis_unit_test(test_dispatcher)
gredis_unit_test(test_hash_table)
gredis_unit_test(test_parse)
gredis_unit_test(test_database)
gredis_unit_test(test_timer_queue)
gredis_unit_test(test_avl_tree)
gredis_unit_test(test_zset)
gredis_unit_test(test_thread_pool)
gredis_unit_test(test_crc32)
gredis_unit_test(test_snapshot)
# needs the corrupt fixture files under tests/data/*.grds
target_compile_definitions(test_snapshot PRIVATE GREDIS_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

# these two live outside gredis_core, so compile their .cpp directly
# instead of linking the lib
add_executable(test_loadgen_histogram tests/unit/test_loadgen_histogram.cpp bench/loadgen/histogram.cpp)
target_include_directories(test_loadgen_histogram PRIVATE tests/unit bench/loadgen)
add_test(NAME test_loadgen_histogram COMMAND test_loadgen_histogram)
set_tests_properties(test_loadgen_histogram PROPERTIES TIMEOUT 120)

add_executable(test_reply_parser tests/unit/test_reply_parser.cpp bench/loadgen/reply_parser.cpp)
target_include_directories(test_reply_parser PRIVATE tests/unit bench/loadgen)
add_test(NAME test_reply_parser COMMAND test_reply_parser)
set_tests_properties(test_reply_parser PROPERTIES TIMEOUT 120)

# benchmarks: build release, run manually, never in ctest/CI
add_executable(bench_micro_hash_table bench/micro_hash_table.cpp)
target_link_libraries(bench_micro_hash_table PRIVATE gredis_core)

# epoll client for mixed GET:SET / sustained-TTL workloads redis-benchmark
# can't express. reuses EventLoop/Fd/Buffer/resp writer from gredis_core
add_executable(bench_loadgen
    bench/loadgen/loadgen.cpp
    bench/loadgen/histogram.cpp
    bench/loadgen/reply_parser.cpp
)
target_link_libraries(bench_loadgen PRIVATE gredis_core)

# manual only, e.g.
#   cmake -B build-fuzz -DCMAKE_CXX_COMPILER=clang++ -DGREDIS_SANITIZE=fuzzer
#   cmake --build build-fuzz --target fuzz_gredis
#   ./build-fuzz/fuzz_gredis -max_total_time=600 tests/fuzz/corpus/
if(GREDIS_SANITIZE STREQUAL "fuzzer")
    add_executable(fuzz_gredis tests/fuzz/fuzz_target.cpp)
    target_link_libraries(fuzz_gredis PRIVATE gredis_core)
    target_link_options(fuzz_gredis PRIVATE -fsanitize=fuzzer)
endif()

# points at the exact binary this build just made, so it never runs a stale one
add_custom_target(integration
    COMMAND ${CMAKE_COMMAND} -E env GREDIS_SERVER_BIN=$<TARGET_FILE:gredis-server>
            python3 -m unittest discover -s ${CMAKE_SOURCE_DIR}/tests/integration -p "test_*.py" -v
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/tests/integration
    DEPENDS gredis-server
    USES_TERMINAL
    VERBATIM
)
