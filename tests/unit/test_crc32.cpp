#include "util/crc32.h"

#include <cstdint>
#include <cstring>
#include <string>

#include "test_harness.h"

using gredis::crc32;

TEST(crc32_of_empty_input_is_zero) {
    CHECK_EQ(crc32(nullptr, 0), uint32_t{0});
}

TEST(crc32_matches_the_standard_check_value) {
    // "123456789" -> 0xCBF43926 is the canonical CRC32/IEEE check value
    const char* s = "123456789";
    CHECK_EQ(crc32(reinterpret_cast<const uint8_t*>(s), std::strlen(s)), uint32_t{0xCBF43926});
}

TEST(crc32_detects_a_single_bit_flip) {
    const std::string original = "the quick brown fox";
    std::string mutated = original;
    mutated[5] ^= 0x01;
    const uint32_t a = crc32(reinterpret_cast<const uint8_t*>(original.data()), original.size());
    const uint32_t b = crc32(reinterpret_cast<const uint8_t*>(mutated.data()), mutated.size());
    CHECK(a != b);
}

TEST(crc32_is_deterministic) {
    const std::string s = "gredis snapshot";
    const uint32_t a = crc32(reinterpret_cast<const uint8_t*>(s.data()), s.size());
    const uint32_t b = crc32(reinterpret_cast<const uint8_t*>(s.data()), s.size());
    CHECK_EQ(a, b);
}

TEST_MAIN()
