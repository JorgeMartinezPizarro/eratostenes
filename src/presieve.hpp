#pragma once
// Pre-sieve: the multiples of the primes 7..163 come from precomputed bit
// patterns that fill each segment (in place of zeroing it), instead of being
// crossed off: these primes hit every segment thousands of times, per-hit
// work no scheduling removes.
//
// For a fixed set of primes, the pattern of wheel indices they mark is
// periodic with period WHEEL_SIZE * product(primes) (wheel_number(k) mod p
// depends only on k mod p*WHEEL_SIZE). One table for all 35 primes would be
// astronomically long, so they are split into 16 small groups (primesieve's
// grouping), one table each, OR-ed together at fill time (primesieve ANDs:
// its set bit means "candidate", here it means "composite").

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

#include "wheel.hpp"

// The groups, primesieve's own (src/PreSieveTables.hpp): three triples of
// the smallest primes, then pairs of a mid-size prime with a large one, so
// every product stays around 6000-10000 bytes of period. All 16 tables:
// ~123 KB of periods plus one PRESIEVE_CHUNK_BYTES tail each (~190 KB).
// fill() costs a pass per group whatever its primes, which is why coverage
// stops at 163 (docs/RESEARCH.md#extending-pre-sieve-coverage-past-prime-163-tried-three-ways-all-reverted).
inline const std::vector<std::vector<uint64_t>> PRESIEVE_GROUPS = {
    {7, 23, 37},
    {11, 19, 31},
    {13, 17, 29},
    {41, 163},
    {43, 157},
    {47, 151},
    {53, 149},
    {59, 139},
    {61, 137},
    {67, 131},
    {71, 127},
    {73, 113},
    {79, 109},
    {83, 107},
    {89, 103},
    {97, 101},
};

// fill() works through dst in chunks of this many bytes, so a table only
// needs one period plus one chunk (+ slack) to serve any window without
// wrapping mid-loop -- see Presieve::fill.
constexpr uint64_t PRESIEVE_CHUNK_BYTES = 4096;

struct PresieveTable {
    uint64_t period_k = 0;       // WHEEL_SIZE * product(this group's primes)
    uint64_t period_bytes = 0;   // period_k / 8
    std::vector<uint64_t> words; // period_k + one fill chunk + slack bits; bit=1 => composite
    const uint8_t* bytes() const { return reinterpret_cast<const uint8_t*>(words.data()); }
};

struct Presieve {
    std::vector<PresieveTable> tables;
    // wheel_index(p) of each pre-sieve prime: the one absolute position
    // where the periodic pattern is wrong (p marked as a multiple of
    // itself). The tables keep the bit (every other position sharing it is
    // a real composite); fill() clears just that position.
    std::vector<uint64_t> self_k;
    // max(self_k): past it no segment can contain one, so fill() skips the
    // correction everywhere but at the very start of the range.
    uint64_t max_self_k = 0;

    // Fills the first `count` bits of dst (room for ceil(count/64) words)
    // with the pre-sieve pattern of the segment starting at wheel index
    // k_low: the OR of every table at its own offset. The tiers OR their
    // marks on top.
    //
    // k_low is a multiple of 64 and every period a multiple of 8, so each
    // table's window starts on a byte: one unaligned 8-byte load per output
    // word. Tables combine 4 at a time (dst written 4 times, not 16), and dst
    // is covered in PRESIEVE_CHUNK_BYTES chunks, each table's offset wrapping
    // by its period in between, so a table is one period plus one chunk long.
    __attribute__((noinline)) void fill(uint64_t* dst, uint64_t k_low, uint64_t count) const {
        constexpr uint64_t CHUNK_WORDS = PRESIEVE_CHUNK_BYTES / 8;
        const uint64_t words_needed = (count + 63) / 64;
        const size_t nt = tables.size();
        uint64_t off[MAX_TABLES]; // byte offset of the current chunk in each table's period
        for (size_t t = 0; t < nt; ++t) off[t] = (k_low % tables[t].period_k) >> 3; // multiple of 8, see above

        for (uint64_t w0 = 0; w0 < words_needed; w0 += CHUNK_WORDS) {
            const uint64_t n = std::min(CHUNK_WORDS, words_needed - w0);
            uint64_t* d = dst + w0;
            size_t t = 0;
            bool first = true;
            for (; t + 4 <= nt; t += 4) {
                const uint8_t* s0 = tables[t + 0].bytes() + off[t + 0];
                const uint8_t* s1 = tables[t + 1].bytes() + off[t + 1];
                const uint8_t* s2 = tables[t + 2].bytes() + off[t + 2];
                const uint8_t* s3 = tables[t + 3].bytes() + off[t + 3];
                if (first) {
                    for (uint64_t i = 0; i < n; ++i) d[i] = load_u64(s0, i) | load_u64(s1, i) | load_u64(s2, i) | load_u64(s3, i);
                    first = false;
                } else {
                    for (uint64_t i = 0; i < n; ++i) d[i] |= load_u64(s0, i) | load_u64(s1, i) | load_u64(s2, i) | load_u64(s3, i);
                }
            }
            // Fewer than 4 tables left (none with PRESIEVE_GROUPS' 16).
            for (; t < nt; ++t) {
                const uint8_t* s = tables[t].bytes() + off[t];
                if (first) {
                    for (uint64_t i = 0; i < n; ++i) d[i] = load_u64(s, i);
                    first = false;
                } else {
                    for (uint64_t i = 0; i < n; ++i) d[i] |= load_u64(s, i);
                }
            }
            for (size_t u = 0; u < nt; ++u) {
                off[u] += PRESIEVE_CHUNK_BYTES;
                while (off[u] >= tables[u].period_bytes) off[u] -= tables[u].period_bytes;
            }
        }

        // The pre-sieve primes themselves, only in the first segments.
        if (k_low <= max_self_k) {
            for (uint64_t sk : self_k) {
                if (sk >= k_low && sk < k_low + count) {
                    uint64_t idx = sk - k_low;
                    dst[idx >> 6] &= ~(1ULL << (idx & 63));
                }
            }
        }
    }

    // fill() keeps its per-table offsets on the stack (checked by build_presieve).
    static constexpr size_t MAX_TABLES = 32;

private:
    static uint64_t load_u64(const uint8_t* base, uint64_t word_idx) {
        uint64_t v;
        std::memcpy(&v, base + word_idx * 8, sizeof(v));
        return v;
    }
};

// Builds one table for a single group of primes (all coprime with WHEEL_MOD).
inline PresieveTable build_presieve_table(const std::vector<uint64_t>& primes,
                                           std::vector<uint64_t>& self_k_out) {
    PresieveTable tbl;
    uint64_t period_k = static_cast<uint64_t>(WHEEL_SIZE);
    for (uint64_t p : primes) period_k *= p;
    tbl.period_k = period_k;
    tbl.period_bytes = period_k / 8;

    // +one fill chunk: any byte offset in [0, period) plus a chunk-sized
    // window (see Presieve::fill) still lands inside the buffer, no
    // wraparound needed within a chunk. +128: slack past the last word.
    uint64_t total_bits = period_k + PRESIEVE_CHUNK_BYTES * 8 + 128;
    tbl.words.assign((total_bits + 63) / 64, 0);

    // Every multiple of p on the wheel from m = 1 on, not from p*p: a table
    // only holds its own group's primes, so the smaller multiples are not
    // covered by anything else. m = 1 marks p itself; fill() clears that
    // one position (self_k).
    for (uint64_t p : primes) {
        auto delta = compute_wheel_deltas(p);
        uint64_t m = 1;
        uint64_t r = m % WHEEL_MOD;
        uint64_t step = STEP_TO_COPRIME[r];
        m += step;
        r += step;
        if (r >= WHEEL_MOD) r -= WHEEL_MOD;
        int j = WHEEL_POS[r];
        uint64_t k = wheel_index(p * m);

        while (k < total_bits) {
            tbl.words[k >> 6] |= (1ULL << (k & 63));
            k += delta[j];
            j = (j + 1) & (WHEEL_SIZE - 1);
        }
        self_k_out.push_back(wheel_index(p));
    }
    return tbl;
}

// Builds one table per group in `groups` (every prime in them is >= 7, so
// coprime to the wheel).
inline Presieve build_presieve(const std::vector<std::vector<uint64_t>>& groups) {
    Presieve ps;
    for (const auto& group : groups) ps.tables.push_back(build_presieve_table(group, ps.self_k));
    if (ps.tables.size() > Presieve::MAX_TABLES) {
        throw std::runtime_error("build_presieve: too many presieve groups");
    }
    for (uint64_t sk : ps.self_k) ps.max_self_k = std::max(ps.max_self_k, sk);
    return ps;
}
