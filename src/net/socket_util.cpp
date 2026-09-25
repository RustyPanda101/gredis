#include "net/socket_util.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#include "util/log.h"

namespace gredis {

namespace {

[[noreturn]] void die(const char* what) {
    LOG_ERROR("%s: %s", what, std::strerror(errno));
    std::exit(1);
}

} // namespace

Fd create_listener(const std::string& ip, uint16_t port, int backlog) {
    Fd fd(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    if (!fd) {
        die("socket()");
    }

    const int reuse = 1;
    if (::setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        die("setsockopt(SO_REUSEADDR)");
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
        LOG_ERROR("invalid bind address '%s'", ip.c_str());
        std::exit(1);
    }

    if (::bind(fd.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
        die("bind()");
    }

    if (::listen(fd.get(), backlog) < 0) {
        die("listen()");
    }

    return fd;
}

void set_tcp_nodelay(int fd) {
    const int val = 1;
    if (::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &val, sizeof(val)) < 0) {
        LOG_WARN("setsockopt(TCP_NODELAY) on fd=%d failed: %s", fd, std::strerror(errno));
    }
}

} // namespace gredis
