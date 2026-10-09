#pragma once
// Byte encoding for the gap between two consecutive primes, used by the
// .db (SQLite + zstd) output mode. Shared verbatim by the writer
// (gap_block_sink.hpp) and the reader (nth_prime.cpp) so encode and
// decode can never drift apart. Unchanged since .db format version 2.
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
// Why wheel indices: the wheel gap is close to geometric and independent
// between primes, which zstd's Huffman stage codes within ~1% of its
// entropy; an integer gap carries the residue-class structure zstd can't
// see. See docs/RESEARCH.md#gap-encoding-wheel-index-deltas.

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "wheel.hpp"

inline bool on_wheel(uint64_t n) { return n >= FIRST_WHEEL_PRIME; }

// The most bytes one gap takes (an escape).
constexpr size_t MAX_GAP_BYTES = 5;

// Writes the gap from prev to next at out; returns the end of what it wrote.
inline uint8_t* encode_gap(uint64_t prev, uint64_t next, uint8_t* out) {
    if (on_wheel(prev)) {
        const uint64_t dk = wheel_index(next) - wheel_index(prev);
        if (dk <= 255) {
            *out = static_cast<uint8_t>(dk);
            return out + 1;
        }
    }
    *out = 0;
    const uint32_t d32 = static_cast<uint32_t>(next - prev);
    std::memcpy(out + 1, &d32, sizeof(d32));
    return out + MAX_GAP_BYTES;
}

// Decodes a whole block: `first` (the block's start_prime) and the count - 1
// gaps after it in data[0, size), into out[0, count). The wheel index is
// carried from prime to prime, so a 1-byte gap costs one wheel_number (a
// shift, a multiply, a table load) instead of a wheel_index division per
// prime. Returns false if the gaps don't fill the block exactly (corrupt).
inline bool decode_block(const uint8_t* data, size_t size, uint64_t first, uint64_t count, uint64_t* out) {
    if (count == 0) return size == 0;
    uint64_t v = first;
    bool wheel = on_wheel(v);
    uint64_t k = wheel ? wheel_index(v) : 0;
    out[0] = v;
    size_t pos = 0;
    for (uint64_t i = 1; i < count; ++i) {
        if (pos >= size) return false;
        const uint8_t b = data[pos++];
        if (b != 0) {
            if (!wheel) return false; // a 1-byte gap is only written after an on-wheel prime
            k += b;
            v = wheel_number(k);
        } else {
            if (size - pos < 4) return false;
            uint32_t d32;
            std::memcpy(&d32, data + pos, sizeof(d32));
            pos += sizeof(d32);
            v += d32;
            wheel = on_wheel(v);
            if (wheel) k = wheel_index(v);
        }
        out[i] = v;
    }
    return pos == size;
}
