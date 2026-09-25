#include "persistence/snapshot.h"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <memory>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "storage/value.h"
#include "util/crc32.h"
#include "util/log.h"

namespace gredis {

namespace {

constexpr uint16_t kFormatVersion = 1;
constexpr uint8_t kEofTag = 0xFF;
constexpr size_t kHeaderSize = 16; // "GRDS" + u16 version + u16 flags + u64 created_unix_ms
constexpr size_t kFooterSize = 13; // u8 eof tag + u64 record_count + u32 crc32

constexpr uint8_t kTypeString = 1;
constexpr uint8_t kTypeHash = 2;
constexpr uint8_t kTypeList = 3;
constexpr uint8_t kTypeSet = 4;
constexpr uint8_t kTypeZSet = 5;

// little-endian, byte-by-byte with shifts (not memcpy of a struct) --
// avoids padding/endianness/alignment issues

void write_u8(std::vector<uint8_t>& out, uint8_t v) { out.push_back(v); }

void write_u16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v));
    out.push_back(static_cast<uint8_t>(v >> 8));
}

void write_u32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<uint8_t>(v >> (8 * i)));
    }
}

void write_u64(std::vector<uint8_t>& out, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<uint8_t>(v >> (8 * i)));
    }
}

void write_i64(std::vector<uint8_t>& out, int64_t v) { write_u64(out, static_cast<uint64_t>(v)); }

// memcpy into a same-sized uint to view the bit pattern -- reinterpret_cast
// would be strict-aliasing UB
void write_f64(std::vector<uint8_t>& out, double v) {
    uint64_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    write_u64(out, bits);
}

void write_bytes(std::vector<uint8_t>& out, const void* data, size_t len) {
    const auto* p = static_cast<const uint8_t*>(data);
    out.insert(out.end(), p, p + len);
}

void write_bytes(std::vector<uint8_t>& out, std::string_view s) { write_bytes(out, s.data(), s.size()); }

// u32 length prefix + that many raw bytes
void write_lp_bytes(std::vector<uint8_t>& out, std::string_view s) {
    write_u32(out, static_cast<uint32_t>(s.size()));
    write_bytes(out, s);
}

// each checks remaining length before reading, advances offset only on success

bool read_u8(std::span<const uint8_t> data, size_t& offset, uint8_t& out) {
    if (offset + 1 > data.size()) {
        return false;
    }
    out = data[offset];
    offset += 1;
    return true;
}

bool read_u16(std::span<const uint8_t> data, size_t& offset, uint16_t& out) {
    if (offset + 2 > data.size()) {
        return false;
    }
    out = static_cast<uint16_t>(static_cast<uint16_t>(data[offset]) |
                                 static_cast<uint16_t>(static_cast<uint16_t>(data[offset + 1]) << 8));
    offset += 2;
    return true;
}

bool read_u32(std::span<const uint8_t> data, size_t& offset, uint32_t& out) {
    if (offset + 4 > data.size()) {
        return false;
    }
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
        v |= static_cast<uint32_t>(data[offset + static_cast<size_t>(i)]) << (8 * i);
    }
    out = v;
    offset += 4;
    return true;
}

bool read_u64(std::span<const uint8_t> data, size_t& offset, uint64_t& out) {
    if (offset + 8 > data.size()) {
        return false;
    }
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<uint64_t>(data[offset + static_cast<size_t>(i)]) << (8 * i);
    }
    out = v;
    offset += 8;
    return true;
}

bool read_i64(std::span<const uint8_t> data, size_t& offset, int64_t& out) {
    uint64_t bits = 0;
    if (!read_u64(data, offset, bits)) {
        return false;
    }
    out = static_cast<int64_t>(bits);
    return true;
}

bool read_f64(std::span<const uint8_t> data, size_t& offset, double& out) {
    uint64_t bits = 0;
    if (!read_u64(data, offset, bits)) {
        return false;
    }
    std::memcpy(&out, &bits, sizeof(out));
    return true;
}

// bounds check here is what stops a corrupt length (e.g. 0xFFFFFFFF)
// from causing a multi-GiB allocation
bool read_lp_bytes(std::span<const uint8_t> data, size_t& offset, std::string& out) {
    uint32_t len = 0;
    if (!read_u32(data, offset, len)) {
        return false;
    }
    if (offset + len > data.size()) {
        return false;
    }
    out.assign(reinterpret_cast<const char*>(data.data() + offset), len);
    offset += len;
    return true;
}

// Value& not const&: ZSet::tree() isn't const-qualified, and encoding
// a zset walks the tree to visit members in order
void encode_payload(std::vector<uint8_t>& out, Value& value) {
    switch (value.index()) {
        case 0: { // string
            write_lp_bytes(out, std::get<std::string>(value));
            return;
        }
        case 1: { // hash
            const HashValue& h = *std::get<std::unique_ptr<HashValue>>(value);
            write_u32(out, static_cast<uint32_t>(h.size()));
            h.for_each([&out](const std::string& field, const std::string& val) {
                write_lp_bytes(out, field);
                write_lp_bytes(out, val);
            });
            return;
        }
        case 2: { // list
            const ListValue& l = *std::get<std::unique_ptr<ListValue>>(value);
            write_u32(out, static_cast<uint32_t>(l.size()));
            for (const std::string& item : l) { // head -> tail order (deque front -> back)
                write_lp_bytes(out, item);
            }
            return;
        }
        case 3: { // set
            const SetValue& s = *std::get<std::unique_ptr<SetValue>>(value);
            write_u32(out, static_cast<uint32_t>(s.size()));
            s.for_each([&out](const std::string& member, const Empty&) { write_lp_bytes(out, member); });
            return;
        }
        case 4: { // zset
            ZSet& z = *std::get<std::unique_ptr<ZSet>>(value);
            write_u32(out, static_cast<uint32_t>(z.size()));
            for (ZSet::Node* n = z.tree().select(0); n != nullptr; n = ZSet::Tree::next(n)) {
                write_lp_bytes(out, n->key.member);
                write_f64(out, n->key.score);
            }
            return;
        }
    }
}

uint8_t type_tag_of(const Value& value) {
    switch (value.index()) {
        case 0:
            return kTypeString;
        case 1:
            return kTypeHash;
        case 2:
            return kTypeList;
        case 3:
            return kTypeSet;
        case 4:
            return kTypeZSet;
    }
    return 0; // unreachable, every alternative handled above
}

// decodes one record's payload (the part after the key) into a fresh
// Value of the type `tag` names
bool decode_payload(std::span<const uint8_t> data, size_t& offset, uint8_t tag, Value& out, std::string& err) {
    switch (tag) {
        case kTypeString: {
            std::string s;
            if (!read_lp_bytes(data, offset, s)) {
                err = "string payload: length points past end of file";
                return false;
            }
            out = std::move(s);
            return true;
        }
        case kTypeHash: {
            uint32_t count = 0;
            if (!read_u32(data, offset, count)) {
                err = "hash payload: truncated field count";
                return false;
            }
            auto h = std::make_unique<HashValue>();
            for (uint32_t i = 0; i < count; ++i) {
                std::string field;
                std::string val;
                if (!read_lp_bytes(data, offset, field) || !read_lp_bytes(data, offset, val)) {
                    err = "hash payload: field/value length points past end of file";
                    return false;
                }
                h->insert_or_assign(std::move(field), std::move(val));
            }
            out = std::move(h);
            return true;
        }
        case kTypeList: {
            uint32_t count = 0;
            if (!read_u32(data, offset, count)) {
                err = "list payload: truncated element count";
                return false;
            }
            auto l = std::make_unique<ListValue>();
            for (uint32_t i = 0; i < count; ++i) {
                std::string item;
                if (!read_lp_bytes(data, offset, item)) {
                    err = "list payload: element length points past end of file";
                    return false;
                }
                l->push_back(std::move(item)); // encode wrote front->back, this reproduces it
            }
            out = std::move(l);
            return true;
        }
        case kTypeSet: {
            uint32_t count = 0;
            if (!read_u32(data, offset, count)) {
                err = "set payload: truncated member count";
                return false;
            }
            auto s = std::make_unique<SetValue>();
            for (uint32_t i = 0; i < count; ++i) {
                std::string member;
                if (!read_lp_bytes(data, offset, member)) {
                    err = "set payload: member length points past end of file";
                    return false;
                }
                s->insert_or_assign(std::move(member), Empty{});
            }
            out = std::move(s);
            return true;
        }
        case kTypeZSet: {
            uint32_t count = 0;
            if (!read_u32(data, offset, count)) {
                err = "zset payload: truncated member count";
                return false;
            }
            auto z = std::make_unique<ZSet>();
            for (uint32_t i = 0; i < count; ++i) {
                std::string member;
                double score = 0.0;
                if (!read_lp_bytes(data, offset, member) || !read_f64(data, offset, score)) {
                    err = "zset payload: member/score length points past end of file";
                    return false;
                }
                z->upsert(std::move(member), score);
            }
            out = std::move(z);
            return true;
        }
        default:
            err = "unknown record type tag";
            return false;
    }
}

// decodes one record (type, has_ttl, [expiry], key, payload) into scratch
bool decode_one_record(std::span<const uint8_t> data, size_t& offset, Database& scratch, int64_t now_mono,
                        int64_t now_unix, std::string& err) {
    uint8_t type = 0;
    if (!read_u8(data, offset, type)) {
        err = "truncated record: missing type tag";
        return false;
    }
    if (type < kTypeString || type > kTypeZSet) {
        err = "unknown record type tag";
        return false;
    }

    uint8_t has_ttl = 0;
    if (!read_u8(data, offset, has_ttl)) {
        err = "truncated record: missing has_ttl flag";
        return false;
    }
    if (has_ttl != 0 && has_ttl != 1) {
        err = "invalid has_ttl flag value (must be 0 or 1)";
        return false;
    }

    std::optional<int64_t> expire_unix_ms;
    if (has_ttl == 1) {
        int64_t v = 0;
        if (!read_i64(data, offset, v)) {
            err = "truncated record: missing expire time";
            return false;
        }
        expire_unix_ms = v;
    }

    std::string key;
    if (!read_lp_bytes(data, offset, key)) {
        err = "truncated record: key length points past end of file";
        return false;
    }

    Value value;
    if (!decode_payload(data, offset, type, value, err)) {
        return false;
    }

    scratch.load_key(std::move(key), std::move(value), expire_unix_ms, now_mono, now_unix);
    return true;
}

} // namespace

std::vector<uint8_t> encode_snapshot(Database& db, int64_t now_mono, int64_t now_unix) {
    std::vector<uint8_t> out;

    write_bytes(out, "GRDS", 4);
    write_u16(out, kFormatVersion);
    write_u16(out, 0); // flags, reserved
    write_u64(out, static_cast<uint64_t>(now_unix));

    uint64_t record_count = 0;
    db.for_each_for_snapshot(now_mono, now_unix,
                              [&](const std::string& key, Value& value, std::optional<int64_t> expire_unix_ms) {
                                  write_u8(out, type_tag_of(value));
                                  write_u8(out, expire_unix_ms ? 1 : 0);
                                  if (expire_unix_ms) {
                                      write_i64(out, *expire_unix_ms);
                                  }
                                  write_lp_bytes(out, key);
                                  encode_payload(out, value);
                                  ++record_count;
                              });

    write_u8(out, kEofTag);
    write_u64(out, record_count);
    const uint32_t crc = crc32(out.data(), out.size());
    write_u32(out, crc);
    return out;
}

SnapshotLoadResult decode_snapshot(std::span<const uint8_t> bytes, Database& out, int64_t now_mono,
                                    int64_t now_unix) {
    SnapshotLoadResult result;

    if (bytes.size() < kHeaderSize + kFooterSize) {
        result.error = "file too small to contain a valid header and footer";
        return result;
    }
    if (bytes[0] != 'G' || bytes[1] != 'R' || bytes[2] != 'D' || bytes[3] != 'S') {
        result.error = "bad magic (not a gredis snapshot file)";
        return result;
    }

    size_t offset = 4;
    uint16_t version = 0;
    uint16_t flags = 0;
    uint64_t created_unix_ms = 0;
    read_u16(bytes, offset, version);         // bounds already checked above
    read_u16(bytes, offset, flags);           // reserved
    read_u64(bytes, offset, created_unix_ms); // unused by the loader
    (void)flags;
    (void)created_unix_ms;
    if (version != kFormatVersion) {
        result.error = "unsupported snapshot format version " + std::to_string(version);
        return result;
    }

    const size_t footer_start = bytes.size() - kFooterSize;
    if (bytes[footer_start] != kEofTag) {
        result.error = "corrupt or missing footer (EOF marker not found)";
        return result;
    }
    size_t footer_offset = footer_start + 1;
    uint64_t stored_record_count = 0;
    uint32_t stored_crc = 0;
    read_u64(bytes, footer_offset, stored_record_count); // bounds already covered by kFooterSize
    read_u32(bytes, footer_offset, stored_crc);

    const uint32_t computed_crc = crc32(bytes.data(), bytes.size() - 4);
    if (computed_crc != stored_crc) {
        result.error = "checksum mismatch (file is corrupt or truncated)";
        return result;
    }

    Database scratch;
    uint64_t parsed_records = 0;
    while (offset < footer_start) {
        std::string record_err;
        if (!decode_one_record(bytes, offset, scratch, now_mono, now_unix, record_err)) {
            result.error = "record " + std::to_string(parsed_records) + ": " + record_err;
            return result;
        }
        ++parsed_records;
    }
    if (offset != footer_start) {
        result.error = "a record's length ran past the footer boundary";
        return result;
    }
    if (parsed_records != stored_record_count) {
        result.error = "record count mismatch (footer says " + std::to_string(stored_record_count) + ", parsed " +
                        std::to_string(parsed_records) + ")";
        return result;
    }

    out = std::move(scratch);
    result.status = SnapshotLoadStatus::Ok;
    return result;
}

namespace {

// small RAII close, local to this file -- no need to pull in util/fd.h's
// Fd (that's for net/'s long-lived fds)
class ScopedFd {
public:
    explicit ScopedFd(int fd) : fd_(fd) {}
    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;
    ~ScopedFd() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }
    int get() const { return fd_; }
    bool valid() const { return fd_ >= 0; }

private:
    int fd_;
};

std::string errno_message(const char* what) { return std::string(what) + ": " + std::strerror(errno); }

} // namespace

bool write_snapshot_file(const std::string& path, const std::vector<uint8_t>& bytes, std::string& error) {
    const std::string tmp_path = path + ".tmp";

    const int raw_fd = ::open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (raw_fd < 0) {
        error = errno_message("open(tmp file)");
        return false;
    }
    ScopedFd fd(raw_fd);

    size_t written = 0;
    while (written < bytes.size()) {
        const ssize_t n = ::write(fd.get(), bytes.data() + written, bytes.size() - written);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            error = errno_message("write()");
            ::unlink(tmp_path.c_str());
            return false;
        }
        written += static_cast<size_t>(n);
    }

    if (::fsync(fd.get()) != 0) {
        error = errno_message("fsync(tmp file)");
        ::unlink(tmp_path.c_str());
        return false;
    }

    if (::rename(tmp_path.c_str(), path.c_str()) != 0) {
        error = errno_message("rename()");
        ::unlink(tmp_path.c_str());
        return false;
    }

    // rename already succeeded, so a dir-fsync failure here is just
    // logged, not treated as SAVE/BGSAVE failing
    std::error_code ec;
    std::filesystem::path dir = std::filesystem::path(path).parent_path();
    if (dir.empty()) {
        dir = ".";
    }
    const int dir_fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
    if (dir_fd >= 0) {
        ScopedFd dfd(dir_fd);
        if (::fsync(dfd.get()) != 0) {
            LOG_WARN("fsync() on snapshot directory '%s' failed: %s", dir.c_str(), std::strerror(errno));
        }
    } else {
        LOG_WARN("open() on snapshot directory '%s' for fsync failed: %s", dir.c_str(), std::strerror(errno));
    }

    return true;
}

bool read_snapshot_file(const std::string& path, std::vector<uint8_t>& out, std::string& error) {
    const int raw_fd = ::open(path.c_str(), O_RDONLY);
    if (raw_fd < 0) {
        error = errno_message("open()");
        return false;
    }
    ScopedFd fd(raw_fd);

    struct stat st{};
    if (::fstat(fd.get(), &st) != 0) {
        error = errno_message("fstat()");
        return false;
    }

    out.resize(static_cast<size_t>(st.st_size));
    size_t total = 0;
    while (total < out.size()) {
        const ssize_t n = ::read(fd.get(), out.data() + total, out.size() - total);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            error = errno_message("read()");
            return false;
        }
        if (n == 0) {
            break; // shouldn't happen given fstat's size, but avoid spinning forever
        }
        total += static_cast<size_t>(n);
    }
    out.resize(total);
    return true;
}

} // namespace gredis
