// binary snapshot codec: Database <-> bytes, plus the atomic write path.
// knows about storage/ but nothing about sockets/RESP/dispatcher.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "storage/database.h"

namespace gredis {

// now_mono/now_unix must be the same instant on each clock -- used to
// convert TTLs to the persisted unix-ms form and skip already-expired
// keys. takes Database& not const& because ZSet's tree walk isn't const.
std::vector<uint8_t> encode_snapshot(Database& db, int64_t now_mono, int64_t now_unix);

enum class SnapshotLoadStatus { Ok, Error };

struct SnapshotLoadResult {
    SnapshotLoadStatus status = SnapshotLoadStatus::Error;
    std::string error; // populated only when status == Error
};

// validates magic/version/footer/CRC32 before parsing any record, then
// parses into a scratch Database and only moves it into `out` once the
// whole file parsed ok -- so a failed load leaves `out` untouched.
// now_mono/now_unix convert each record's TTL back to monotonic time,
// same as encode_snapshot() but reversed.
SnapshotLoadResult decode_snapshot(std::span<const uint8_t> bytes, Database& out, int64_t now_mono,
                                    int64_t now_unix);

// atomic write: write to <path>.tmp, fsync, rename over path, then
// best-effort fsync the directory. on failure, removes the tmp file
// and leaves the old snapshot untouched.
bool write_snapshot_file(const std::string& path, const std::vector<uint8_t>& bytes, std::string& error);

// missing file (ENOENT) is also an error here -- callers that want "no
// file yet" to be fine (main.cpp at startup) check existence themselves first
bool read_snapshot_file(const std::string& path, std::vector<uint8_t>& out, std::string& error);

} // namespace gredis
