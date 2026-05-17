#pragma once

#include <cstdint>
#include <cstring>

namespace vortex {

inline uint64_t xxhash64(const void* data, size_t len, uint64_t seed = 0) {
    constexpr uint64_t P1 = 0x9E3779B185EBCA87ULL;
    constexpr uint64_t P2 = 0xC2B2AE3D27D4EB4FULL;
    constexpr uint64_t P3 = 0x165667B19E3779F9ULL;
    constexpr uint64_t P4 = 0x85EBCA77C2B2AE63ULL;
    constexpr uint64_t P5 = 0x27D4EB2F165667C5ULL;

    const uint8_t* p = static_cast<const uint8_t*>(data);
    const uint8_t* end = p + len;

    uint64_t h64;
    if (len >= 32) {
        uint64_t v1 = seed + P1 + P2;
        uint64_t v2 = seed + P2;
        uint64_t v3 = seed + 0;
        uint64_t v4 = seed - P1;

        const uint8_t* limit = end - 32;
        do {
            uint64_t k1, k2, k3, k4;
            std::memcpy(&k1, p, 8); p += 8;
            std::memcpy(&k2, p, 8); p += 8;
            std::memcpy(&k3, p, 8); p += 8;
            std::memcpy(&k4, p, 8); p += 8;

            v1 = ((v1 << 31) | (v1 >> 33)) + P2;
            v1 = v1 * P1 + k1;
            v2 = ((v2 << 31) | (v2 >> 33)) + P2;
            v2 = v2 * P1 + k2;
            v3 = ((v3 << 31) | (v3 >> 33)) + P2;
            v3 = v3 * P1 + k3;
            v4 = ((v4 << 31) | (v4 >> 33)) + P2;
            v4 = v4 * P1 + k4;
        } while (p <= limit);

        h64 = ((v1 << 1) | (v1 >> 63))
            + ((v2 << 7) | (v2 >> 57))
            + ((v3 << 12) | (v3 >> 52))
            + ((v4 << 18) | (v4 >> 46));

        v1 = (((v1 << 31) | (v1 >> 33)) * P1) ^ (((v1 << 31) | (v1 >> 33)) * P1);
        v1 = v1 * P1 + v4;
        v2 = v2 * P2 + v3;
        h64 = h64 + v1 + v2;
    } else {
        h64 = seed + P5;
    }

    h64 += len;

    while (p + 8 <= end) {
        uint64_t k1;
        std::memcpy(&k1, p, 8); p += 8;
        k1 = k1 * P2;
        k1 = (k1 << 31) | (k1 >> 33);
        k1 = k1 * P1;
        h64 = h64 ^ k1;
        h64 = (h64 << 27) | (h64 >> 37);
        h64 = h64 * P1 + P4;
    }
    if (p + 4 <= end) {
        uint32_t k1;
        std::memcpy(&k1, p, 4); p += 4;
        uint64_t k = static_cast<uint64_t>(k1) * P1;
        k = (k << 23) | (k >> 41);
        h64 = h64 ^ (k * P2);
        h64 = (h64 << 27) | (h64 >> 37);
        h64 = h64 * P1 + P3;
    }
    while (p < end) {
        h64 = h64 ^ (static_cast<uint64_t>(*p) * P5);
        h64 = (h64 << 11) | (h64 >> 53);
        h64 = h64 * P1;
        ++p;
    }

    h64 = h64 ^ (h64 >> 33);
    h64 = h64 * P2;
    h64 = h64 ^ (h64 >> 29);
    h64 = h64 * P3;
    h64 = h64 ^ (h64 >> 32);

    return h64;
}

}  // namespace vortex
