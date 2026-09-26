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
// that hit's position relative to the current segment's start -- a byte
// index for the small tier (cross_off_all), a bit (wheel) index for the
// medium tier (cross_off_medium). qp < 2^26 (p < ~2e9) is checked by
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
template <int PR>
inline void cross_off(uint8_t* s, uint64_t end, uint64_t qp, uint64_t& i_io, uint32_t& j_io) {
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
        cross_off<PR>(s, end, qp, i, j);
        st->qw = static_cast<uint32_t>((qp << 6) | (PR << 3) | j);
        st->pos = static_cast<uint32_t>(i - rebase);
    }
}

// Medium tier: primes with only a handful of hits per segment, where the
// unrolled loop above can't amortize its per-prime entry cost (an
// unpredictable jump into the switch, plus an unpredictable exit point --
// measured ~5.6x the branch misses of this loop at N=1e11, net slower
// despite 38% fewer instructions). Generic one-hit-per-iteration stepping
// instead, division-free via the shared mod-210 table (wheel210_big.hpp),
// on bit positions; mispredicts only once per prime (the loop exit).
//
// Every medium-tier prime is always > 163 (presieve's {7,23,37} group,
// presieve.hpp, always covers 7 first), so any hit whose multiplier is a
// multiple of 7 is redundant -- already marked composite by 7's own
// presieve pattern. Stepping through only the 48/210 multiplier phases
// coprime to 210 instead of the 8/30 coprime to 30 (GAP_K210/
// ONFLY_CORRECTION210, wheel210_big.hpp -- same derivation as
// ONFLY_CORRECTION/GAP_K in wheel.hpp, just with M210 standing in for
// WHEEL_R) skips ~14% of candidate hits in this tier -- the same trick
// already validated and kept for the sparse tier's own big-wheel table.
// This does NOT touch DenseState's layout or split the medium tier's
// single flat list -- only the shared constant tables grew a little (64
// entries -> 48+8*48, still ~1.7KB, still trivially L1-resident) -- so
// there's no cache-vs-instructions trade being made here. A mod-2310
// version of the same idea was considered and rejected (candidate
// reduction not worth the L1 pressure, and incompatible with this tier's
// packed DenseState layout for the sparse tier's own half of it -- see
// docs/RESEARCH.md).
//
// PR is a compile-time template parameter (matching primesieve's own
// EratMedium split into crossOff_7/11/13/.../31, one per residue class):
// one list per class (medium_[8] in segment_sieve.hpp, same shape as the
// small tier's small_[8]) instead of one flat list carrying a runtime
// `ri`. big::ONFLY_CORRECTION210[PR] is a compile-time-constant row
// offset (foldable into the load's displacement) instead of a per-prime
// runtime-computed pointer -- removes one multiply-by-row-size per prime
// per segment. qw packs (qp << 6) | w (w needs 6 bits, 0..47, same budget
// as the small tier's (pr<<3)|j).
//
// This DOES split medium_ into 8 lists -- a much smaller fragmentation
// than the (reverted) 64-list restructuring tried for this tier, and each
// list still holds every prime of its OWN class permanently (no
// per-segment migration between lists, since a prime's class never
// changes) -- structurally identical to how small_[8] already works
// without issue. A chained-table variant, a mod-2310 extension, a 4-way
// interleaved stepping variant, and the 64-list restructuring itself were
// all tried and reverted -- see docs/RESEARCH.md for the numbers.
template <int PR>
inline void cross_off_medium(uint64_t* words, uint64_t end_bit, DenseState* first, DenseState* last, uint64_t rebase_bits) {
    const uint32_t* gap_k = big::GAP_K210.data();
    const uint32_t* corr = big::ONFLY_CORRECTION210[PR].data();
    for (DenseState* st = first; st != last; ++st) {
        uint64_t k = st->pos;
        uint64_t qp = st->qw >> 6;
        uint32_t w = st->qw & 63;
        while (k < end_bit) {
            words[k >> 6] |= uint64_t{1} << (k & 63);
            k += qp * gap_k[w] + corr[w];
            w = (w + 1 == 48) ? 0 : w + 1;
        }
        st->qw = static_cast<uint32_t>((qp << 6) | w);
        st->pos = static_cast<uint32_t>(k - rebase_bits);
    }
}

// A 2-ahead software-prefetch variant of this loop, and an EratMedium-style
// 64-list restructuring keyed by (class, entry phase), were both tried and
// reverted -- real wins at some N that failed to hold up (or reversed
// outright) at this project's actual E13-E14 target range. See
// docs/RESEARCH.md for the full measurements.

} // namespace erat
