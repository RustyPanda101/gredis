// epoll RESP2 load generator for mixed GET/SET and TTL workloads redis-benchmark can't do.
// Reuses gredis_core's EventLoop/Fd/Buffer/RESP writer -- it's just another client.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "histogram.h"
#include "reply_parser.h"
#include "net/buffer.h"
#include "net/event_loop.h"
#include "net/socket_util.h"
#include "protocol/resp_writer.h"
#include "util/fd.h"
#include "util/log.h"
#include "util/parse.h"

namespace {

using Clock = std::chrono::steady_clock;
using gredis::Buffer;
using gredis::EventLoop;
using gredis::Fd;
using gredis::Readiness;
using gredis::loadgen::LatencyHistogram;
using gredis::loadgen::parse_reply;
using gredis::loadgen::ReplyParseStatus;

// cap reads per wakeup so one bad connection can't starve the rest
constexpr size_t kReadChunkSize = 16 * 1024;
constexpr int kMaxRecvsPerWakeup = 4;

struct Options {
    std::string host = "127.0.0.1";
    uint16_t port = 6380;
    int connections = 50;
    int pipeline = 1;
    int keyspace = 10000;
    int value_size = 3;
    int get_pct = 50;  // rest are SET
    int expire_ms = 0; // if > 0, every SET carries PX expire_ms
    double duration_sec = 10.0;
    int64_t requests = 0; // if > 0, overrides duration_sec
    unsigned seed = 1;
};

[[noreturn]] void usage_and_exit(const char* argv0, int code) {
    std::fprintf(stderr,
        "Usage: %s [options]\n"
        "  --host <ip>              default 127.0.0.1\n"
        "  --port <n>               default 6380\n"
        "  --connections|-c <n>     default 50\n"
        "  --pipeline|-P <n>        requests in flight per connection, default 1\n"
        "  --keyspace|-r <n>        distinct keys, default 10000\n"
        "  --value-size|-d <n>      SET payload size in bytes, default 3\n"
        "  --get-pct <0-100>        percent of ops that are GET, rest are SET, default 50\n"
        "  --expire-ms <n>          PX <n> on every SET (0 = no TTL), default 0\n"
        "  --duration-sec <s>       run for this many seconds, default 10\n"
        "  --requests <n>           total requests instead of a duration (0 = use duration)\n"
        "  --seed <n>               PRNG seed, default 1\n"
        "Prints one CSV header line and one CSV data line to stdout.\n",
        argv0);
    std::exit(code);
}

int64_t require_int(const std::string& s, const char* argv0, const char* name) {
    const auto v = gredis::parse_int64(s);
    if (!v) {
        std::fprintf(stderr, "loadgen: invalid value for %s: '%s'\n", name, s.c_str());
        usage_and_exit(argv0, 2);
    }
    return *v;
}

double require_double(const std::string& s, const char* argv0, const char* name) {
    char* end = nullptr;
    errno = 0;
    const double v = std::strtod(s.c_str(), &end);
    if (errno != 0 || end == s.c_str() || *end != '\0') {
        std::fprintf(stderr, "loadgen: invalid value for %s: '%s'\n", name, s.c_str());
        usage_and_exit(argv0, 2);
    }
    return v;
}

std::optional<Options> parse_args(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) usage_and_exit(argv[0], 2);
            return argv[++i];
        };
        if (a == "--host") {
            o.host = next();
        } else if (a == "--port") {
            o.port = static_cast<uint16_t>(require_int(next(), argv[0], "--port"));
        } else if (a == "--connections" || a == "-c") {
            o.connections = static_cast<int>(require_int(next(), argv[0], "--connections"));
        } else if (a == "--pipeline" || a == "-P") {
            o.pipeline = static_cast<int>(require_int(next(), argv[0], "--pipeline"));
        } else if (a == "--keyspace" || a == "-r") {
            o.keyspace = static_cast<int>(require_int(next(), argv[0], "--keyspace"));
        } else if (a == "--value-size" || a == "-d") {
            o.value_size = static_cast<int>(require_int(next(), argv[0], "--value-size"));
        } else if (a == "--get-pct") {
            o.get_pct = static_cast<int>(require_int(next(), argv[0], "--get-pct"));
        } else if (a == "--expire-ms") {
            o.expire_ms = static_cast<int>(require_int(next(), argv[0], "--expire-ms"));
        } else if (a == "--duration-sec") {
            o.duration_sec = require_double(next(), argv[0], "--duration-sec");
        } else if (a == "--requests") {
            o.requests = require_int(next(), argv[0], "--requests");
        } else if (a == "--seed") {
            o.seed = static_cast<unsigned>(require_int(next(), argv[0], "--seed"));
        } else if (a == "--help" || a == "-h") {
            usage_and_exit(argv[0], 0);
        } else {
            std::fprintf(stderr, "loadgen: unknown option '%s'\n", a.c_str());
            usage_and_exit(argv[0], 2);
        }
    }
    if (o.connections <= 0 || o.pipeline <= 0 || o.keyspace <= 0 || o.value_size < 0 ||
        o.get_pct < 0 || o.get_pct > 100 || o.expire_ms < 0 || o.duration_sec <= 0) {
        return std::nullopt;
    }
    return o;
}

struct GlobalState {
    explicit GlobalState(Options opts_in) : opts(std::move(opts_in)), rng(opts.seed) {}

    bool stop_issuing() const {
        if (opts.requests > 0) {
            return issued >= opts.requests;
        }
        const double elapsed = std::chrono::duration<double>(Clock::now() - start_time).count();
        return elapsed >= opts.duration_sec;
    }

    Options opts;
    std::mt19937 rng;
    std::string value;
    Clock::time_point start_time;
    int64_t issued = 0;
    int64_t completed = 0;
    int64_t errors = 0;
    int connect_failures = 0;
    LatencyHistogram hist;
};

struct ConnState {
    Fd fd;
    bool primed = false; // connect() done and initial pipeline sent
    bool closed = false;
    Buffer out;
    Buffer in;
    // replies come back in the order requests were sent, so front of
    // this queue always matches the next reply
    std::deque<Clock::time_point> inflight_started;
};

Fd connect_nonblocking(const std::string& host, uint16_t port) {
    Fd fd(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    if (!fd) {
        LOG_ERROR("loadgen: socket(): %s", std::strerror(errno));
        std::exit(1);
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        LOG_ERROR("loadgen: invalid host '%s'", host.c_str());
        std::exit(1);
    }
    const int rc = ::connect(fd.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (rc < 0 && errno != EINPROGRESS) {
        LOG_ERROR("loadgen: connect(): %s", std::strerror(errno));
        std::exit(1);
    }
    // treat sync connect (rc==0) and EINPROGRESS the same: wait for
    // EPOLLOUT and check SO_ERROR there
    return fd;
}

void append_request(Buffer& out, const Options& opts, const std::string& value,
                     std::mt19937& rng) {
    std::uniform_int_distribution<int> pct(1, 100);
    std::uniform_int_distribution<int> key_dist(0, opts.keyspace - 1);
    const std::string key = "key:" + std::to_string(key_dist(rng));

    if (pct(rng) <= opts.get_pct) {
        gredis::resp::array_header(out, 2);
        gredis::resp::bulk(out, "GET");
        gredis::resp::bulk(out, key);
    } else if (opts.expire_ms > 0) {
        gredis::resp::array_header(out, 5);
        gredis::resp::bulk(out, "SET");
        gredis::resp::bulk(out, key);
        gredis::resp::bulk(out, value);
        gredis::resp::bulk(out, "PX");
        gredis::resp::bulk(out, std::to_string(opts.expire_ms));
    } else {
        gredis::resp::array_header(out, 3);
        gredis::resp::bulk(out, "SET");
        gredis::resp::bulk(out, key);
        gredis::resp::bulk(out, value);
    }
}

void attempt_write(ConnState& conn) {
    while (!conn.out.empty()) {
        const auto span = conn.out.readable();
        const ssize_t sent = ::send(conn.fd.get(), span.data(), span.size(), MSG_NOSIGNAL);
        if (sent > 0) {
            conn.out.consume(static_cast<size_t>(sent));
            continue;
        }
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        }
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        LOG_WARN("loadgen: send() failed on fd=%d: %s", conn.fd.get(), std::strerror(errno));
        conn.closed = true;
        break;
    }
}

void update_interest(ConnState& conn, EventLoop& loop) {
    if (conn.closed) {
        return;
    }
    loop.modify(conn.fd.get(), /*readable=*/conn.primed, /*writable=*/!conn.out.empty());
}

// keeps `pipeline` requests in flight until the requests/duration budget is hit
void top_up_pipeline(ConnState& conn, GlobalState& g) {
    while (conn.inflight_started.size() < static_cast<size_t>(g.opts.pipeline) &&
           !g.stop_issuing()) {
        append_request(conn.out, g.opts, g.value, g.rng);
        // timestamp at enqueue, not send(): keeps one timestamp per
        // request instead of per (possibly coalesced) send() call
        conn.inflight_started.push_back(Clock::now());
        ++g.issued;
    }
}

void on_readable(ConnState& conn, GlobalState& g) {
    for (int reads = 0; reads < kMaxRecvsPerWakeup; ++reads) {
        char* dst = conn.in.prepare_write(kReadChunkSize);
        const ssize_t n = ::recv(conn.fd.get(), dst, kReadChunkSize, 0);
        if (n > 0) {
            conn.in.commit_written(static_cast<size_t>(n));
            if (static_cast<size_t>(n) < kReadChunkSize) {
                break;
            }
            continue;
        }
        conn.in.commit_written(0);
        if (n == 0) {
            conn.closed = true;
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        LOG_WARN("loadgen: recv() failed on fd=%d: %s", conn.fd.get(), std::strerror(errno));
        conn.closed = true;
        return;
    }

    for (;;) {
        const auto readable = conn.in.readable();
        const auto result =
            parse_reply(std::span<const char>(readable.data(), readable.size()));
        if (result.status == ReplyParseStatus::Incomplete) {
            break;
        }
        if (result.status == ReplyParseStatus::Error) {
            LOG_WARN("loadgen: malformed reply from fd=%d, closing", conn.fd.get());
            conn.closed = true;
            return;
        }
        conn.in.consume(result.consumed);

        if (conn.inflight_started.empty()) {
            LOG_WARN("loadgen: reply on fd=%d with nothing in flight, closing", conn.fd.get());
            conn.closed = true;
            return;
        }
        const Clock::time_point started = conn.inflight_started.front();
        conn.inflight_started.pop_front();
        const auto latency_us =
            std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - started).count();
        g.hist.record(latency_us);
        ++g.completed;
        if (result.is_error) {
            ++g.errors;
        }
    }

    top_up_pipeline(conn, g);
}

void on_writable(ConnState& conn, GlobalState& g) {
    if (!conn.primed) {
        int err = 0;
        socklen_t len = sizeof(err);
        if (::getsockopt(conn.fd.get(), SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0) {
            LOG_WARN("loadgen: connect() to %s:%u failed: %s", g.opts.host.c_str(),
                      static_cast<unsigned>(g.opts.port), std::strerror(err != 0 ? err : errno));
            ++g.connect_failures;
            conn.closed = true;
            return;
        }
        gredis::set_tcp_nodelay(conn.fd.get());
        conn.primed = true;
        top_up_pipeline(conn, g);
    }
    attempt_write(conn);
}

void print_results_csv(const GlobalState& g, double wall_sec) {
    const double throughput = wall_sec > 0.0 ? static_cast<double>(g.completed) / wall_sec : 0.0;
    std::printf("connections,pipeline,keyspace,value_size,get_pct,expire_ms,duration_sec,"
                "requests_completed,errors,connect_failures,throughput_rps,"
                "p50_us,p95_us,p99_us,max_us,mean_us\n");
    std::printf("%d,%d,%d,%d,%d,%d,%.3f,%lld,%lld,%d,%.2f,%lld,%lld,%lld,%lld,%.2f\n",
                g.opts.connections, g.opts.pipeline, g.opts.keyspace, g.opts.value_size,
                g.opts.get_pct, g.opts.expire_ms, wall_sec, static_cast<long long>(g.completed),
                static_cast<long long>(g.errors), g.connect_failures, throughput,
                static_cast<long long>(g.hist.percentile(0.50)),
                static_cast<long long>(g.hist.percentile(0.95)),
                static_cast<long long>(g.hist.percentile(0.99)),
                static_cast<long long>(g.hist.max_us()), g.hist.mean_us());
}

} // namespace

int main(int argc, char** argv) {
    const auto opts_opt = parse_args(argc, argv);
    if (!opts_opt) {
        std::fprintf(stderr, "loadgen: invalid options (see --help)\n");
        return 2;
    }

    GlobalState g(*opts_opt);
    g.value = std::string(static_cast<size_t>(g.opts.value_size), 'x');

    EventLoop loop;
    std::vector<ConnState> conns(static_cast<size_t>(g.opts.connections));
    std::unordered_map<int, ConnState*> by_fd;
    for (auto& conn : conns) {
        conn.fd = connect_nonblocking(g.opts.host, g.opts.port);
        loop.add(conn.fd.get(), /*readable=*/false, /*writable=*/true);
        by_fd[conn.fd.get()] = &conn;
    }

    g.start_time = Clock::now();

    for (;;) {
        const bool any_active = std::any_of(conns.begin(), conns.end(), [](const ConnState& c) {
            return !c.closed && (!c.primed || !c.inflight_started.empty());
        });
        if (!any_active) {
            break;
        }

        loop.run_once(/*timeout_ms=*/1000, [&](int fd, Readiness r) {
            auto it = by_fd.find(fd);
            if (it == by_fd.end()) {
                return;
            }
            ConnState& conn = *it->second;
            if (conn.closed) {
                return;
            }
            if (r.readable) {
                on_readable(conn, g);
            }
            if (!conn.closed && r.writable) {
                on_writable(conn, g);
            }
            if (conn.closed) {
                loop.remove(fd);
                conn.fd.reset();
            } else {
                update_interest(conn, loop);
            }
        });
    }

    const double wall_sec = std::chrono::duration<double>(Clock::now() - g.start_time).count();
    print_results_csv(g, wall_sec);

    if (g.connect_failures > 0) {
        std::fprintf(stderr, "loadgen: %d/%d connections failed to connect\n",
                     g.connect_failures, g.opts.connections);
    }
    return (g.errors > 0 || g.connect_failures == g.opts.connections) ? 1 : 0;
}
