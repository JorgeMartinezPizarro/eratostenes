#pragma once
// Pre-sieve: precomputed bit patterns for the smallest base primes, used to
// fill a new segment's bit array via bitwise combination instead of running
// their marking loop every segment.
//
// The bucket sieve (SegmentSieve) already skips a prime for any segment
// where it has no multiple -- but the smallest base primes (7, 11, 13...)
// have a multiple in *every* segment (their period is far smaller than the
// segment width), so bucket routing never skips them: their marking loop
// (read delta, advance, set bit) runs in full every single segment. That's
// real per-bit work, not scheduling overhead, so bucket routing can't
// remove it.
//
// What can: for a fixed small set of primes, the pattern of which
// wheel-indices they mark composite is periodic (period = WHEEL_SIZE *
// product(primes), since they're pairwise coprime -- proof: wheel_number(k)
// mod p only depends on k mod (p*WHEEL_SIZE), because advancing k by
// p*WHEEL_SIZE advances q = k/WHEEL_SIZE by exactly p, adding a multiple of
// p*WHEEL_MOD, which vanishes mod p). Precompute that pattern once, and
// filling a segment becomes a bulk copy from the precomputed buffer instead
// of per-prime, per-hit marking.
//
// One table for *all* pre-sieve primes at once hits the same wall as the
// wheel itself (wheel.hpp): the period is the *product* of every prime in
// it, so it blows up fast -- {7,11,13,17,19,23} alone is already a ~7.1MB
// table (near the edge of L3, shared read-only across every thread), and
// adding just one more prime (29) would multiply that by 29. primesieve
// hits the same wall and sidesteps it the same way this does: several
// small, independent tables, each covering only a handful of primes (so
// each one's own period -- and thus its own size -- stays tiny), combined
// with a bitwise OR at fill time instead of being multiplied together into
// one giant table. Grouping {7,11,13,17,19,23} into two tables instead of
// one already shrinks their combined size from ~7.1MB to ~8.4KB; from
// there, more (small) tables buy more prime coverage for cheap.
//
// (primesieve's own tables combine with AND instead of OR because their
// convention is the opposite of this project's: a set bit there means
// "still a candidate", so surviving *every* table's filter is the AND of
// all of them. Here a set bit means "composite", so a number that's
// composite according to *any* table's primes is composite overall -- OR.)

#include <cstdint>
#include <cstring>
#include <vector>

#include "wheel.hpp"

// Groups of pre-sieve primes: each group's own period is WHEEL_SIZE *
// product(that group's primes), so keeping groups small keeps every
// table's size small regardless of how many groups there are. Primes
// already covered by the active WHEEL_PRIMES config are dropped
// automatically in build_presieve (e.g. 7 for mod 210+).
//
// This is primesieve's own grouping (src/PreSieveTables.hpp), reused as-is
// rather than re-derived: the first 3 groups triple up the smallest primes
// (7..37) with products balanced around ~6000; the rest pair a mid-size
// prime with a large one specifically so each pair's product also lands
// around 6000-10000 (e.g. 41*163=6683, 97*101=9797) instead of ballooning
// as primes grow -- naively grouping consecutive primes instead (7,11,13 /
// 17,19,23 / ...) hits multi-megabyte tables by the time it reaches
// primes past ~130. All 16 tables combined: ~123KB.
// Extending coverage past 163 was tried three ways (two group-count-
// preserving pairings, and one "just use a huge table" attempt) and all
// three measured a regression, not a win -- see docs/RESEARCH.md.
// Presieve::fill() does one shift-and-OR pass per *group* over the whole
// segment every single segment, a cost that's fixed per group regardless
// of that group's table size or which primes are in it, so a real win
// here would need fewer new groups than primes added, or restructuring
// fill() so a group's cost scales with how often its primes actually hit
// rather than a fixed full-segment pass -- out of scope for what's been
// tried so far.
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

struct PresieveTable {
    uint64_t period_k = 0;       // WHEEL_SIZE * product(this group's primes)
    std::vector<uint64_t> words; // period_k + max_seg_k_width + slack bits; bit=1 => composite
};

struct Presieve {
    std::vector<PresieveTable> tables;
    // wheel_index(p) for each pre-sieve prime, across every group -- the
    // one absolute position where the periodic pattern is wrong (see
    // build_presieve): it reads as composite (p is, trivially, a multiple
    // of itself) but is actually prime. Every OTHER position sharing that
    // bit mod that table's period is a real composite (p times a
    // genuinely larger cofactor) and must stay marked, so the buffer
    // itself is left uncorrected; fill() patches only the exact absolute
    // position, not the periodic bit.
    std::vector<uint64_t> self_k;

    // Fills the first `count` bits of dst (word-granular, dst must have
    // room for ceil(count/64) words) with the pre-sieve pattern for the
    // segment starting at wheel-index k_low: the bitwise OR of every
    // table's own (independently offset) pattern. Replaces
    // zero-initializing the segment's bit array: bucket-scheduled and flat
    // primes above the pre-sieve depth then OR their own marks on top,
    // same as before.
    //
    // k_low is always a multiple of 64 (every chunk and segment start is,
    // see main.cpp/split_ranges), and every table's period_k is always a
    // multiple of WHEEL_SIZE=8 (build_presieve_table). So bit_start =
    // k_low % period_k is always a multiple of 8 -- never just any bit,
    // always a whole byte -- even though it's essentially never a multiple
    // of 64 (a whole word). That means each table's window can be read
    // with one unaligned 8-byte load per output word instead of two
    // aligned word loads shifted and OR'd together to reassemble it
    // (`memcpy` here isn't a function call -- with a compile-time-constant
    // size 8, the compiler folds it into a single unaligned mov, which x86
    // takes no penalty for outside crossing a cache line). Tables are also
    // grouped 4 at a time, so `dst` is written once per group of 4 instead
    // of once per table (16 tables -> 4 writes instead of 16); each
    // group's 4 loads plus 3 ORs per output word is still simple enough
    // for the compiler to auto-vectorize across words, same as before.
    void fill(uint64_t* dst, uint64_t k_low, uint64_t count) const {
        uint64_t words_needed = (count + 63) / 64;
        size_t t = 0;
        bool first = true;

        for (; t + 4 <= tables.size(); t += 4) {
            const uint8_t* s0 = byte_ptr(tables[t + 0], k_low);
            const uint8_t* s1 = byte_ptr(tables[t + 1], k_low);
            const uint8_t* s2 = byte_ptr(tables[t + 2], k_low);
            const uint8_t* s3 = byte_ptr(tables[t + 3], k_low);
            if (first) {
                for (uint64_t i = 0; i < words_needed; ++i) dst[i] = load_u64(s0, i) | load_u64(s1, i) | load_u64(s2, i) | load_u64(s3, i);
                first = false;
            } else {
                for (uint64_t i = 0; i < words_needed; ++i) dst[i] |= load_u64(s0, i) | load_u64(s1, i) | load_u64(s2, i) | load_u64(s3, i);
            }
        }
        // Tail: fewer than 4 tables left (PRESIEVE_GROUPS is 16 long, an
        // exact multiple of 4, but a smaller/custom group list wouldn't be).
        for (; t < tables.size(); ++t) {
            const uint8_t* s = byte_ptr(tables[t], k_low);
            if (first) {
                for (uint64_t i = 0; i < words_needed; ++i) dst[i] = load_u64(s, i);
                first = false;
            } else {
                for (uint64_t i = 0; i < words_needed; ++i) dst[i] |= load_u64(s, i);
            }
        }

        // self_k is tiny (one entry per pre-sieve prime) and only ever
        // actually falls inside k_low==0's segment, but checking
        // unconditionally is cheap and doesn't need that assumption.
        for (uint64_t sk : self_k) {
            if (sk >= k_low && sk < k_low + count) {
                uint64_t idx = sk - k_low;
                dst[idx >> 6] &= ~(1ULL << (idx & 63));
            }
        }
    }

private:
    static const uint8_t* byte_ptr(const PresieveTable& tbl, uint64_t k_low) {
        uint64_t bit_start = k_low % tbl.period_k; // always a multiple of 8, see fill()'s comment
        return reinterpret_cast<const uint8_t*>(tbl.words.data()) + (bit_start >> 3);
    }
    static uint64_t load_u64(const uint8_t* base, uint64_t word_idx) {
        uint64_t v;
        std::memcpy(&v, base + word_idx * 8, sizeof(v));
        return v;
    }
};

// Builds one table for a single group of primes (all coprime with
// WHEEL_MOD, already filtered by build_presieve below).
inline PresieveTable build_presieve_table(const std::vector<uint64_t>& primes,
                                           uint64_t max_seg_k_width,
                                           std::vector<uint64_t>& self_k_out) {
    PresieveTable tbl;
    uint64_t period_k = static_cast<uint64_t>(WHEEL_SIZE);
    for (uint64_t p : primes) period_k *= p;
    tbl.period_k = period_k;

    // +max_seg_k_width: any bit_start in [0, period_k) plus a segment-sized
    // window still lands inside the buffer, no wraparound needed. +128:
    // the shifted-word read touches one word past the last one fill()
    // logically needs.
    uint64_t total_bits = period_k + max_seg_k_width + 128;
    tbl.words.assign((total_bits + 63) / 64, 0);

    // Mark every wheel-representable multiple of p, starting at m=1 (value
    // p itself), not p*p: SegmentSieve starts at p*p because on a full
    // sieve, smaller multiples are already covered by *other* base primes
    // below p -- a guarantee this buffer doesn't have, since a single
    // group only ever marks its own handful of primes, nothing smaller.
    // Marking from m=1 also marks p itself (m=1 -> value p) at
    // k0=wheel_index(p) -- that single bit is wrong only at that one
    // absolute position (see self_k in Presieve: every later position
    // sharing the same bit mod period_k is a genuine composite and must
    // stay marked, so it's *not* cleared here).
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
            if constexpr (WHEEL_SIZE_IS_POW2) {
                j = (j + 1) & (WHEEL_SIZE - 1);
            } else {
                ++j;
                if (j == WHEEL_SIZE) j = 0;
            }
        }
        self_k_out.push_back(wheel_index(p));
    }
    return tbl;
}

// Builds one table per group in `groups`, dropping primes already covered
// by the active WHEEL_PRIMES config (e.g. 7 is dropped for mod 210+) and
// dropping any group that ends up empty as a result.
inline Presieve build_presieve(const std::vector<std::vector<uint64_t>>& groups,
                                uint64_t max_seg_k_width) {
    Presieve ps;
    for (const auto& group : groups) {
        std::vector<uint64_t> filtered;
        for (uint64_t p : group) if (p >= FIRST_WHEEL_PRIME) filtered.push_back(p);
        if (filtered.empty()) continue;
        ps.tables.push_back(build_presieve_table(filtered, max_seg_k_width, ps.self_k));
    }
    return ps;
}
