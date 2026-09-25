#include "net/buffer.h"

namespace gredis {

void Buffer::append(std::string_view s) {
    maybe_compact();
    data_.append(s);
}

void Buffer::consume(size_t n) {
    read_pos_ += n;
    if (read_pos_ == data_.size()) {
        clear(); // nothing left to preserve, don't wait for the compact threshold
    }
}

char* Buffer::prepare_write(size_t n) {
    maybe_compact();
    write_mark_ = data_.size();
    data_.resize(write_mark_ + n);
    return data_.data() + write_mark_;
}

void Buffer::commit_written(size_t actual_n) {
    data_.resize(write_mark_ + actual_n);
}

void Buffer::maybe_compact() {
    if (read_pos_ == 0) {
        return;
    }
    if (read_pos_ > data_.capacity() / 2) {
        // shifts unread bytes to front; capacity usually unchanged, no realloc
        data_.erase(0, read_pos_);
        read_pos_ = 0;
    }
}

} // namespace gredis
