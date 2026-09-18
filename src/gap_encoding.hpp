#pragma once
// Byte encoding for the gap between two consecutive primes, used by the
// .db (SQLite + zstd) output mode. Shared verbatim by the writer
// (gap_block_sink.hpp) and the reader (nth_prime.cpp) so encode and
// decode can never drift apart.
//
//   byte b in [1,255] -> delta = 2*b            (covers delta 2..510,
//                                                 i.e. every gap between
//                                                 odd primes up to 510)
//   byte 0             -> escape: next 4 bytes (native byte order,
//                         effectively little-endian on the platforms this
//                         project targets) hold the raw delta as uint32_t
//                         (covers delta=1 -- the only odd gap, 2->3 -- and
//                         any delta > 510)

#include <cstdint>
#include <cstring>
#include <vector>

inline void encode_gap(uint64_t delta, std::vector<uint8_t>& out) {
    if (delta != 0 && delta % 2 == 0 && delta <= 510) {
        out.push_back(static_cast<uint8_t>(delta / 2));
    } else {
        out.push_back(0);
        uint32_t d32 = static_cast<uint32_t>(delta);
        uint8_t buf[4];
        std::memcpy(buf, &d32, sizeof(buf));
        out.insert(out.end(), buf, buf + sizeof(buf));
    }
}

// Decodes one gap starting at data[pos], advances pos past it, returns the
// delta. Caller is responsible for bounds-checking pos against the
// decompressed block size (a well-formed block never reads past its end).
inline uint64_t decode_gap(const uint8_t* data, size_t& pos) {
    uint8_t b = data[pos++];
    if (b == 0) {
        uint32_t d32;
        std::memcpy(&d32, data + pos, sizeof(d32));
        pos += sizeof(d32);
        return d32;
    }
    return static_cast<uint64_t>(b) * 2;
}
