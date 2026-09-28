#pragma once
// Unrolled, byte-addressed crossing-off for the dense tiers (primesieve's
// EratSmall/EratMedium idea, re-derived for this project's layout).
//
// With the mod-30 wheel, wheel index k = q*8 + j means byte q of the
// segment's bit array holds the 8 candidates of [30q, 30q+30) and bit j is
// the one at residue WHEEL_R[j] -- exactly primesieve's byte layout. For a
// prime p = 30*qp + R[pr] and a multiplier m = 30*i + R[j] (coprime with
// 30), p*m lands in
//
//   byte = p*i + qp*R[j] + (R[pr]*R[j]) / 30,   bit = POS[(R[pr]*R[j]) % 30]
//
// so, taking the byte of the m = 30*i + 1 hit as a "cycle base" B, the 8
// hits of one multiplier cycle are at B + qp*(R[j]-1) + C[pr][j], with a
// bit mask M[pr][j] that is a compile-time constant once pr is fixed, and B
// advances by exactly p bytes per cycle. Templating on pr turns the inner
// loop into 8 `s[b + o_j] |= M_j` per cycle: no per-hit phase counter, no
// per-prime delta[] table load, no variable shift -- about 2 instructions
// per hit instead of the ~9-12 of the generic k += delta[j] loop.
//
// Only valid for WHEEL_MOD == 30 (the byte <-> 30-number correspondence is
// what makes the masks constant); the other wheel configs in wheel.hpp
// would need their own derivation.

#include <cstdint>

#include "wheel.hpp"
#include "wheel210_big.hpp"

static_assert(WHEEL_MOD == 30 && WHEEL_SIZE == 8,
              "erat_small.hpp: el marcado desenrollado por bytes solo existe para la rueda mod 30");

namespace erat {

constexpr uint32_t R[8] = {1, 7, 11, 13, 17, 19, 23, 29};

constexpr int pos30(uint32_t x) {
    for (int j = 0; j < 8; ++j) if (R[j] == x) return j;
    return -1;
}
constexpr uint64_t C(int pr, int j) { return R[pr] * R[j] / 30; }
constexpr uint8_t M(int pr, int j) { return static_cast<uint8_t>(1u << pos30(R[pr] * R[j] % 30)); }

// Per-prime state, 8 bytes: qw = (qp << 6) | (pr << 3) | j, with qp = p / 30,
// pr = WHEEL_POS[p % 30] and j the pending hit's multiplier phase; pos is
// that hit's byte position relative to the current segment's start (the
// medium tier packs qw as (qp << 6) | w instead, see cross_off_medium).
// qp < 2^26 (p < ~2e9) is checked by
// SegmentSieve's constructor.
struct DenseState {
    uint32_t qw;
    uint32_t pos;
};

constexpr uint64_t QP_LIMIT = uint64_t{1} << 26;

// Crosses off p = 30*qp + R[PR] in s[0, end), starting at the pending hit
// (i, j); on return (i, j) is the first hit at or past `end`.
//
// Every local here (qp, p, the o[j]s, b, end) provably fits a uint32_t, but
// narrowing them from uint64_t was measured slower, not faster -- see
// docs/RESEARCH.md.
//
// Inlining is pinned, not left to GCC's heuristics: always inlined into
// process_med64 (one copy per class, its inner loop), called out of line
// from the small tier (cross_off_call below). A restructured sieve_and_emit
// once tipped GCC into the opposite for 6 of 8 classes -- med64 calling it,
// the small tier inlining it -- +2.5% cycles at 1e14 on the i5-13500.
template <int PR>
__attribute__((always_inline)) inline void cross_off(uint8_t* s, uint64_t end, uint64_t qp, uint64_t& i_io, uint32_t& j_io) {
    const uint64_t p = 30 * qp + R[PR];
    const uint64_t o0 = C(PR, 0);
    const uint64_t o1 = qp * (R[1] - 1) + C(PR, 1);
    const uint64_t o2 = qp * (R[2] - 1) + C(PR, 2);
    const uint64_t o3 = qp * (R[3] - 1) + C(PR, 3);
    const uint64_t o4 = qp * (R[4] - 1) + C(PR, 4);
    const uint64_t o5 = qp * (R[5] - 1) + C(PR, 5);
    const uint64_t o6 = qp * (R[6] - 1) + C(PR, 6);
    const uint64_t o7 = qp * (R[7] - 1) + C(PR, 7);
    const uint64_t o[8] = {o0, o1, o2, o3, o4, o5, o6, o7};

    uint32_t j = j_io;
    // Cycle base; may "underflow" (wrap) when the pending hit is early in
    // its cycle -- only ever used in sums b + o_j, which are exact mod 2^64.
    uint64_t b = i_io - o[j];

#define ERAT_HIT(J) \
    if (b + o##J >= end) { j = J; goto done; } \
    s[b + o##J] |= M(PR, J);

    switch (j) {
        case 0: ERAT_HIT(0) [[fallthrough]];
        case 1: ERAT_HIT(1) [[fallthrough]];
        case 2: ERAT_HIT(2) [[fallthrough]];
        case 3: ERAT_HIT(3) [[fallthrough]];
        case 4: ERAT_HIT(4) [[fallthrough]];
        case 5: ERAT_HIT(5) [[fallthrough]];
        case 6: ERAT_HIT(6) [[fallthrough]];
        case 7: ERAT_HIT(7)
            b += p;
    }

    // Offsets grow with j, so b + o7 < end means the whole cycle fits.
    while (b + o7 < end) {
        s[b + o0] |= M(PR, 0);
        s[b + o1] |= M(PR, 1);
        s[b + o2] |= M(PR, 2);
        s[b + o3] |= M(PR, 3);
        s[b + o4] |= M(PR, 4);
        s[b + o5] |= M(PR, 5);
        s[b + o6] |= M(PR, 6);
        s[b + o7] |= M(PR, 7);
        b += p;
    }

    ERAT_HIT(0)
    ERAT_HIT(1)
    ERAT_HIT(2)
    ERAT_HIT(3)
    ERAT_HIT(4)
    ERAT_HIT(5)
    ERAT_HIT(6)
    j = 7; // b + o7 >= end is already known from the loop exit
#undef ERAT_HIT

done:
    i_io = b + o[j];
    j_io = j;
}

template <int PR>
__attribute__((noinline)) void cross_off_call(uint8_t* s, uint64_t end, uint64_t qp, uint64_t& i_io, uint32_t& j_io) {
    cross_off<PR>(s, end, qp, i_io, j_io);
}

// Runs every state in [first, last) -- all of residue class PR -- over
// s[0, end), then rebases each pending hit by `rebase` bytes (the
// segment's byte width on its last pass over a segment, 0 otherwise). One
// list per class (see SegmentSieve::small_) so the class dispatch happens
// once per list, not as an unpredictable switch per prime.
template <int PR>
inline void cross_off_class(uint8_t* s, uint64_t end, DenseState* first, DenseState* last, uint64_t rebase) {
    for (DenseState* st = first; st != last; ++st) {
        uint64_t i = st->pos;
        uint64_t qp = st->qw >> 6;
        uint32_t j = st->qw & 7;
        cross_off_call<PR>(s, end, qp, i, j);
        st->qw = static_cast<uint32_t>((qp << 6) | (PR << 3) | j);
        st->pos = static_cast<uint32_t>(i - rebase);
    }
}

// Medium tier: primes with only a handful of hits per segment, where the
// unrolled loop above can't amortize its per-prime entry/exit cost -- see
// docs/RESEARCH.md#small-tiers-unrolled-loop-applied-to-medium-hit-count-primes-measured-not-adopted.
// Generic one-hit-per-iteration stepping instead, division-free via the
// shared mod-210 tables (wheel210_big.hpp), on byte positions.
//
// Every medium-tier prime is always > 163 (presieve's {7,23,37} group
// always covers 7 first), so multiplier phases that are multiples of 7 are
// redundant -- stepping through only the 48/210 phases coprime to 210
// (PACK210, wheel210_big.hpp) instead of the 8/30
// coprime to 30 skips ~14% of candidate hits here, same trick as the
// sparse tier's own big-wheel table. See
// docs/RESEARCH.md#cross_off_medium-mod-210-multiplier-stepping-2026-09-24
// for the numbers, and
// docs/RESEARCH.md#cross_off_medium-mod-2310-stepping-considered-not-implemented-2026-09-25-external-review-opus-55
// for why a mod-2310 extension was considered and rejected.
//
// PR is a compile-time template parameter (matching primesieve's own
// EratMedium split into crossOff_7/11/13/.../31, one per residue class):
// one list per class (medium_[8] in segment_sieve.hpp, same shape as the
// small tier's small_[8]). qw packs (qp << 6) | w (w needs 6 bits, 0..47,
// same budget as the small tier's (pr<<3)|j). See
// docs/RESEARCH.md#cross_off_medium-class-specialized-layout-kept-2026-09-24.
// A chained-table variant, a mod-2310 extension, a 4-way interleaved
// stepping variant, and a 64-list restructuring were all tried and
// reverted -- see docs/RESEARCH.md's other `cross_off_medium` entries.
//
// Byte positions and a per-(class, phase) mask, like the sparse tier, not
// bit positions: marking a bit index costs a shift, a word index and a
// variable shift per hit, a byte index just `s[pos] |= mask` (-12%
// instructions:u and -3.4% cycles:u at 1e13 together with the doubled
// tables below, see docs/RESEARCH.md).
// The tables hold two 48-phase cycles, so w only needs wrapping when it
// reaches 96 -- at most once per call, and never when med64 is on (a
// medium prime then has under 48 hits per segment) -- instead of a
// compare-and-select on every hit.
//
// Out of line on purpose, like every class until GCC started inlining
// some of them into sieve_chunk (see cross_off's own note on pinning).
template <int PR>
__attribute__((noinline)) void cross_off_medium(uint8_t* s, uint64_t end, DenseState* first, DenseState* last, uint64_t rebase) {
    const uint32_t* pack = big::PACK210[PR].data();
    for (DenseState* st = first; st != last; ++st) {
        uint64_t pos = st->pos;
        uint64_t qp = st->qw >> 6;
        uint64_t w = st->qw & 63;
        while (pos < end) {
            uint32_t t = pack[w]; // mask | dm << 8 | corr << 16
            s[pos] |= static_cast<uint8_t>(t);
            pos += qp * ((t >> 8) & 0xff) + (t >> 16);
            if (++w == 96) [[unlikely]] w = 48;
        }
        if (w >= 48) w -= 48;
        st->qw = static_cast<uint32_t>((qp << 6) | w);
        st->pos = static_cast<uint32_t>(pos - rebase);
    }
}

// A 2-ahead software-prefetch variant of this loop, and an EratMedium-style
// 64-list restructuring keyed by (class, entry phase), were both tried and
// reverted -- real wins at some N that failed to hold up (or reversed
// outright) at this project's actual E13-E14 target range. See
// docs/RESEARCH.md for the full measurements.

} // namespace erat
