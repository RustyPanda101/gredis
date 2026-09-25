// RAII fd wrapper, move-only, closes exactly once in the destructor.
// no raw close() calls anywhere else in this codebase.
#pragma once

namespace gredis {

class Fd {
public:
    Fd() noexcept = default;
    explicit Fd(int fd) noexcept : fd_(fd) {}

    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;

    Fd(Fd&& other) noexcept;
    Fd& operator=(Fd&& other) noexcept;

    ~Fd();

    int get() const noexcept { return fd_; }
    bool valid() const noexcept { return fd_ >= 0; }
    explicit operator bool() const noexcept { return valid(); }

    // releases ownership without closing, caller becomes responsible for it
    int release() noexcept;

    void reset(int new_fd = -1) noexcept; // closes current fd (if any), takes new_fd

private:
    int fd_ = -1;
};

} // namespace gredis
