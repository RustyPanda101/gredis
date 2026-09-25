// growable byte buffer with a read cursor, used for both in/out streams
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace gredis {

class Buffer {
public:
    // compacts first if that reclaims space -- needed here too, not just in
    // prepare_write(), or an output buffer built purely with append() never compacts
    void append(std::string_view s);
    void append(const char* data, size_t len) { append(std::string_view(data, len)); }

    // valid until the next non-const call on this Buffer
    std::string_view readable() const {
        return std::string_view(data_).substr(read_pos_);
    }

    bool empty() const { return read_pos_ == data_.size(); }
    size_t size() const { return data_.size() - read_pos_; }
    size_t capacity() const { return data_.capacity(); }

    // n must be <= size(); resets both to 0 for free if the cursor catches up
    void consume(size_t n);

    void clear() {
        data_.clear();
        read_pos_ = 0;
    }

    // returns a pointer to write into (e.g. via recv()); caller must call
    // commit_written() after with how many bytes it actually wrote.
    //
    // grows the string to the max possible size first (resize() zero-fills
    // new chars, so it has to happen before writing real bytes in, not after)
    // then commit_written() shrinks it back down. wastes a zero-fill of up to
    // n bytes per call; resize_and_overwrite (C++23) would avoid it but we're on C++20.
    char* prepare_write(size_t n);
    void commit_written(size_t actual_n);

private:
    void maybe_compact();

    std::string data_;
    size_t read_pos_ = 0;
    size_t write_mark_ = 0; // data_.size() at the start of the current prepare_write()
};

} // namespace gredis
