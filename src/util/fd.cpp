#include "util/fd.h"

#include <unistd.h>

namespace gredis {

Fd::Fd(Fd&& other) noexcept : fd_(other.fd_) {
    other.fd_ = -1;
}

Fd& Fd::operator=(Fd&& other) noexcept {
    if (this != &other) {
        reset();
        fd_ = other.fd_;
        other.fd_ = -1;
    }
    return *this;
}

Fd::~Fd() {
    reset();
}

int Fd::release() noexcept {
    const int f = fd_;
    fd_ = -1;
    return f;
}

void Fd::reset(int new_fd) noexcept {
    if (fd_ >= 0) {
        ::close(fd_);
    }
    fd_ = new_fd;
}

} // namespace gredis
