// standard CRC32 (zlib polynomial 0xEDB88320), used for snapshot file integrity
#pragma once

#include <cstddef>
#include <cstdint>

namespace gredis {

uint32_t crc32(const uint8_t* data, size_t len); // table built once, lazily, on first call

} // namespace gredis
