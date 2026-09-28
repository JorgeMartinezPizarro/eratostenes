#pragma once
// Byte encoding for the gap between two consecutive primes, used by the
// .db (SQLite + zstd) output mode. Shared verbatim by the writer
// (gap_block_sink.hpp) and the reader (nth_prime.cpp) so encode and
// decode can never drift apart. Format version 2 (meta.format_version).
//
//   byte b in [1,255] -> next = wheel_number(wheel_index(prev) + b), i.e.
//                        the gap counted in wheel indices (candidates
//                        coprime to WHEEL_MOD), not in integers
//   byte 0             -> escape: next 4 bytes (native byte order,
//                         effectively little-endian on the platforms this
//                         project targets) hold the raw integer delta as
//                         uint32_t. Used when prev or next is off the wheel
//                         (2, 3, 5 -> 7) and for a wheel gap > 255 (an
//                         integer gap > ~960 on mod 30; never below 1e15)
//
// Why wheel indices: version 1 stored delta/2, whose distribution carries
// the residue-class structure (which gaps are possible from each residue
// mod 30) that zstd can't model without that context; the wheel gap is
// close to geometric and independent between primes, which zstd's Huffman
// stage codes within ~1% of its entropy. Measured on real prime windows,
// zstd -1: 5.16 -> 4.26 bits/prime at 1e12, 5.27 -> 4.39 at 1e13, 5.48 ->
// 4.62 at 1e15. See docs/RESEARCH.md#gap-encoding-wheel-index-deltas.

#include <cstdint>
#include <cstring>
#include <vector>

#include "wheel.hpp"

inline bool on_wheel(uint64_t n) { return n >= FIRST_WHEEL_PRIME; }

inline void encode_gap(uint64_t prev, uint64_t next, std::vector<uint8_t>& out) {
    if (on_wheel(prev)) {
        uint64_t dk = wheel_index(next) - wheel_index(prev);
        if (dk <= 255) {
            out.push_back(static_cast<uint8_t>(dk));
            return;
        }
    }
    out.push_back(0);
    uint32_t d32 = static_cast<uint32_t>(next - prev);
    uint8_t buf[4];
    std::memcpy(buf, &d32, sizeof(buf));
    out.insert(out.end(), buf, buf + sizeof(buf));
}

// Decodes one gap starting at data[pos], advances pos past it, returns the
// prime after `prev`. Caller is responsible for bounds-checking pos against
// the decompressed block size (a well-formed block never reads past its end).
inline uint64_t decode_gap(const uint8_t* data, size_t& pos, uint64_t prev) {
    uint8_t b = data[pos++];
    if (b == 0) {
        uint32_t d32;
        std::memcpy(&d32, data + pos, sizeof(d32));
        pos += sizeof(d32);
        return prev + d32;
    }
    return wheel_number(wheel_index(prev) + b);
}
