#pragma once
// "Sinks" for SegmentSieve: what is done with the primes of each segment.
//
//  - NullSink: count mode (no -o, no --print); sieve_and_emit only counts.
//  - TextSink: --print, the primes as text into a buffer that main.cpp's
//    run_print writes to stdout in order.
//  - GapBlockSink (gap_block_sink.hpp): -o, the .db's compressed blocks.
//
// A sink with write_segment(words, k_low, count) gets every segment whole;
// one without it gets a count only.

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <memory>

#include "wheel.hpp"

struct NullSink {};

// A growable char buffer that leaves what it grows into uninitialized
// (std::vector would zero it before every segment's text is written).
struct TextBuffer {
    std::unique_ptr<char[]> data;
    size_t size = 0;
    size_t cap = 0;

    // Room for `extra` more bytes; returns where they start.
    char* reserve(size_t extra) {
        if (size + extra > cap) {
            const size_t c = std::max(cap * 2, size + extra);
            std::unique_ptr<char[]> d(new char[c]);
            if (size) std::memcpy(d.get(), data.get(), size);
            data = std::move(d);
            cap = c;
        }
        return data.get() + size;
    }
};

struct TextSink {
    TextBuffer& out;

    // Every prime of a sieved segment, one per line: wheel index k_low + i is
    // prime when bit i of `words` is clear, for i < count. Returns how many.
    // The primes are counted first so that room for all of them is made
    // once and the lines go out through a local pointer (see
    // GapBlockSink::write_segment on why locals).
    uint64_t write_segment(const uint64_t* words, uint64_t k_low, uint64_t count) {
        constexpr size_t MAX_LINE = 21; // 20 digits and '\n'
        const uint64_t words_needed = (count + 63) / 64;
        const auto primes_of = [&](uint64_t w) {
            uint64_t bits = ~words[w];
            const uint64_t remaining = count - w * 64; // >= 1 for every w < words_needed
            if (remaining < 64) bits &= (uint64_t{1} << remaining) - 1;
            return bits;
        };
        uint64_t n = 0;
        for (uint64_t w = 0; w < words_needed; ++w) n += static_cast<uint64_t>(__builtin_popcountll(primes_of(w)));
        char* p = out.reserve(n * MAX_LINE);
        for (uint64_t w = 0; w < words_needed; ++w) {
            uint64_t bits = primes_of(w);
            const uint64_t k_word = k_low + w * 64;
            while (bits) {
                const uint64_t k = k_word + static_cast<uint64_t>(__builtin_ctzll(bits));
                bits &= bits - 1;
                p = std::to_chars(p, p + MAX_LINE, (k >> WHEEL_SIZE_LOG2) * WHEEL_MOD + WHEEL_R[k & (WHEEL_SIZE - 1)]).ptr;
                *p++ = '\n';
            }
        }
        out.size = static_cast<size_t>(p - out.data.get());
        return n;
    }
};
