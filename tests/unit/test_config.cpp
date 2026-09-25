#include "test_harness.h"

#include <string>
#include <vector>

#include "config.h"

using gredis::ParseStatus;
using gredis::parse_args;

namespace {

std::vector<char*> to_argv(std::vector<std::string>& args) {
    std::vector<char*> argv;
    argv.reserve(args.size());
    for (auto& a : args) {
        argv.push_back(a.data());
    }
    return argv;
}

} // namespace

TEST(default_config_when_no_args) {
    std::vector<std::string> args = {"gredis-server"};
    auto argv = to_argv(args);
    const auto r = parse_args(static_cast<int>(argv.size()), argv.data());
    CHECK(r.status == ParseStatus::Ok);
    CHECK_EQ(r.config.bind, std::string("127.0.0.1"));
    CHECK_EQ(r.config.port, 6380);
    CHECK_EQ(r.config.maxclients, 10000);
    CHECK_EQ(r.config.idle_timeout_sec, 0);
    CHECK_EQ(r.config.threads, 2);
    CHECK(r.config.snapshot_path.empty());
}

TEST(parses_all_flags) {
    std::vector<std::string> args = {
        "gredis-server", "--bind", "0.0.0.0", "--port", "7000",
        "--maxclients", "50", "--idle-timeout-sec", "30",
        "--threads", "4", "--snapshot", "/tmp/dump.grds",
        "--log-level", "debug",
    };
    auto argv = to_argv(args);
    const auto r = parse_args(static_cast<int>(argv.size()), argv.data());
    CHECK(r.status == ParseStatus::Ok);
    CHECK_EQ(r.config.bind, std::string("0.0.0.0"));
    CHECK_EQ(r.config.port, 7000);
    CHECK_EQ(r.config.maxclients, 50);
    CHECK_EQ(r.config.idle_timeout_sec, 30);
    CHECK_EQ(r.config.threads, 4);
    CHECK_EQ(r.config.snapshot_path, std::string("/tmp/dump.grds"));
    CHECK(r.config.log_level == gredis::LogLevel::Debug);
}

TEST(help_flag_returns_help_requested) {
    std::vector<std::string> args = {"gredis-server", "--help"};
    auto argv = to_argv(args);
    const auto r = parse_args(static_cast<int>(argv.size()), argv.data());
    CHECK(r.status == ParseStatus::HelpRequested);
}

TEST(help_flag_stops_processing_at_the_point_it_appears) {
    // flags processed strictly left to right, no pre-scan for --help --
    // an earlier error is still reported even if --help appears later
    std::vector<std::string> args = {"gredis-server", "--port", "not-a-number", "--help"};
    auto argv = to_argv(args);
    const auto r = parse_args(static_cast<int>(argv.size()), argv.data());
    CHECK(r.status == ParseStatus::Error);
}

TEST(missing_value_is_error) {
    std::vector<std::string> args = {"gredis-server", "--port"};
    auto argv = to_argv(args);
    const auto r = parse_args(static_cast<int>(argv.size()), argv.data());
    CHECK(r.status == ParseStatus::Error);
}

TEST(non_numeric_port_is_error) {
    std::vector<std::string> args = {"gredis-server", "--port", "abc"};
    auto argv = to_argv(args);
    const auto r = parse_args(static_cast<int>(argv.size()), argv.data());
    CHECK(r.status == ParseStatus::Error);
}

TEST(port_zero_is_error) {
    std::vector<std::string> args = {"gredis-server", "--port", "0"};
    auto argv = to_argv(args);
    const auto r = parse_args(static_cast<int>(argv.size()), argv.data());
    CHECK(r.status == ParseStatus::Error);
}

TEST(port_above_65535_is_error) {
    std::vector<std::string> args = {"gredis-server", "--port", "65536"};
    auto argv = to_argv(args);
    const auto r = parse_args(static_cast<int>(argv.size()), argv.data());
    CHECK(r.status == ParseStatus::Error);
}

TEST(port_with_leading_plus_is_error) {
    // strict integer parsing rejects a leading '+'
    std::vector<std::string> args = {"gredis-server", "--port", "+6380"};
    auto argv = to_argv(args);
    const auto r = parse_args(static_cast<int>(argv.size()), argv.data());
    CHECK(r.status == ParseStatus::Error);
}

TEST(port_with_trailing_junk_is_error) {
    std::vector<std::string> args = {"gredis-server", "--port", "6380x"};
    auto argv = to_argv(args);
    const auto r = parse_args(static_cast<int>(argv.size()), argv.data());
    CHECK(r.status == ParseStatus::Error);
}

TEST(negative_idle_timeout_is_error) {
    std::vector<std::string> args = {"gredis-server", "--idle-timeout-sec", "-1"};
    auto argv = to_argv(args);
    const auto r = parse_args(static_cast<int>(argv.size()), argv.data());
    CHECK(r.status == ParseStatus::Error);
}

TEST(zero_idle_timeout_is_ok_and_means_disabled) {
    std::vector<std::string> args = {"gredis-server", "--idle-timeout-sec", "0"};
    auto argv = to_argv(args);
    const auto r = parse_args(static_cast<int>(argv.size()), argv.data());
    CHECK(r.status == ParseStatus::Ok);
    CHECK_EQ(r.config.idle_timeout_sec, 0);
}

TEST(zero_maxclients_is_error) {
    std::vector<std::string> args = {"gredis-server", "--maxclients", "0"};
    auto argv = to_argv(args);
    const auto r = parse_args(static_cast<int>(argv.size()), argv.data());
    CHECK(r.status == ParseStatus::Error);
}

TEST(threads_above_limit_is_error) {
    std::vector<std::string> args = {"gredis-server", "--threads", "65"};
    auto argv = to_argv(args);
    const auto r = parse_args(static_cast<int>(argv.size()), argv.data());
    CHECK(r.status == ParseStatus::Error);
}

TEST(invalid_log_level_is_error) {
    std::vector<std::string> args = {"gredis-server", "--log-level", "verbose"};
    auto argv = to_argv(args);
    const auto r = parse_args(static_cast<int>(argv.size()), argv.data());
    CHECK(r.status == ParseStatus::Error);
}

TEST(client_output_limits_default_soft_below_hard) {
    std::vector<std::string> args = {"gredis-server"};
    auto argv = to_argv(args);
    const auto r = parse_args(static_cast<int>(argv.size()), argv.data());
    CHECK(r.status == ParseStatus::Ok);
    CHECK(r.config.output_soft_limit_bytes <= r.config.output_hard_limit_bytes);
}

TEST(client_output_limits_parse_correctly) {
    std::vector<std::string> args = {
        "gredis-server", "--client-output-soft-limit", "1024",
        "--client-output-hard-limit", "65536",
    };
    auto argv = to_argv(args);
    const auto r = parse_args(static_cast<int>(argv.size()), argv.data());
    CHECK(r.status == ParseStatus::Ok);
    CHECK_EQ(r.config.output_soft_limit_bytes, static_cast<size_t>(1024));
    CHECK_EQ(r.config.output_hard_limit_bytes, static_cast<size_t>(65536));
}

TEST(client_output_soft_limit_above_hard_limit_is_error) {
    std::vector<std::string> args = {
        "gredis-server", "--client-output-soft-limit", "100",
        "--client-output-hard-limit", "50",
    };
    auto argv = to_argv(args);
    const auto r = parse_args(static_cast<int>(argv.size()), argv.data());
    CHECK(r.status == ParseStatus::Error);
}

TEST(client_output_hard_limit_zero_is_error) {
    std::vector<std::string> args = {"gredis-server", "--client-output-hard-limit", "0"};
    auto argv = to_argv(args);
    const auto r = parse_args(static_cast<int>(argv.size()), argv.data());
    CHECK(r.status == ParseStatus::Error);
}

TEST(max_query_buf_parses_correctly) {
    std::vector<std::string> args = {"gredis-server", "--max-query-buf", "4096"};
    auto argv = to_argv(args);
    const auto r = parse_args(static_cast<int>(argv.size()), argv.data());
    CHECK(r.status == ParseStatus::Ok);
    CHECK_EQ(r.config.max_query_buf, static_cast<size_t>(4096));
}

TEST(max_query_buf_zero_is_error) {
    std::vector<std::string> args = {"gredis-server", "--max-query-buf", "0"};
    auto argv = to_argv(args);
    const auto r = parse_args(static_cast<int>(argv.size()), argv.data());
    CHECK(r.status == ParseStatus::Error);
}

TEST(unknown_flag_is_error) {
    std::vector<std::string> args = {"gredis-server", "--frobnicate"};
    auto argv = to_argv(args);
    const auto r = parse_args(static_cast<int>(argv.size()), argv.data());
    CHECK(r.status == ParseStatus::Error);
}

TEST_MAIN()
