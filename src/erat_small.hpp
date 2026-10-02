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

// The mod-30 residues and their bit positions, shared with the mod-210 /
// mod-2310 tables (wheel210_big.hpp).
inline constexpr const uint32_t (&R)[8] = big::R30;
using big::pos30;
constexpr uint64_t C(int pr, int j) { return R[pr] * R[j] / 30; }
constexpr uint8_t M(int pr, int j) { return static_cast<uint8_t>(1u << pos30(R[pr] * R[j] % 30)); }

// Per-prime state, 8 bytes: qw = (qp << 6) | (pr << 3) | j, with qp = p / 30,
// pr = WHEEL_POS[p % 30] and j the pending hit's multiplier phase; pos is
// that hit's byte position relative to the current segment's start (the
// medium tier keeps its state as two arrays instead, see cross_off_medium).
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
// Small tier only (med64 uses cross_off_checked210). Out of line, one call
// per prime from cross_off_class: the layout every small-tier measurement
// was taken on, which GCC stops choosing on its own once med64 no longer
// shares this function.
template <int PR>
__attribute__((noinline)) void cross_off(uint8_t* s, uint64_t end, uint64_t qp, uint64_t& i_io, uint32_t& j_io) {
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

// med64 tier: same contract as cross_off, primesieve EratMedium's loop shape
// -- one running byte index, one bounds check per hit, the switch jumping
// into the middle of the cycle. For med64 (tens of hits per call) the
// per-hit compare runs beside the store the loop is bound by, and the call
// leaves at a single loop exit (~1 mispredict) instead of cross_off's
// unrolled-loop exit plus data-dependent tail exit (~1.65).
// On the mod-210 multiplier wheel (w = 0..47, M210[w]): skips the 1/7 of
// mod-30 hits whose multiplier is a multiple of 7 (7 is always presieved,
// and every med64 prime is > 163). Switch into a for (;;), one check per
// hit -- so there's no per-call offset table
// (what sank the earlier mod-210 med64 attempts): the byte step from phase w
// to w+1 is qp*dm + corr with dm in {2,4,6,8,10}, so 5 multiples of qp in
// registers plus compile-time constants (big::TABLE) cover all 48 cases.
template <int PR>
__attribute__((always_inline)) inline void cross_off_checked210(uint8_t* s, uint64_t end, uint64_t qp, uint64_t& i_io, uint32_t& w_io) {
    const uint64_t q2 = qp * 2, q4 = qp * 4, q8 = qp * 8;
    const uint64_t q6 = q2 + q4, q10 = q2 + q8;
    uint64_t i = i_io;
    uint32_t w = 0; // always set before `done`; the init only silences -Wmaybe-uninitialized

#define ERAT_CHK210(W) \
    case W: { \
        constexpr big::Entry E = big::TABLE[PR * 48 + W]; \
        if (i >= end) { w = W; goto done; } \
        s[i] |= E.mask; \
        i += (E.dm == 2 ? q2 : E.dm == 4 ? q4 : E.dm == 6 ? q6 : E.dm == 8 ? q8 : q10) + E.corr; \
    }

    switch (w_io) {
        for (;;) {
            ERAT_CHK210(0) [[fallthrough]]; ERAT_CHK210(1) [[fallthrough]]; ERAT_CHK210(2) [[fallthrough]];
            ERAT_CHK210(3) [[fallthrough]]; ERAT_CHK210(4) [[fallthrough]]; ERAT_CHK210(5) [[fallthrough]];
            ERAT_CHK210(6) [[fallthrough]]; ERAT_CHK210(7) [[fallthrough]]; ERAT_CHK210(8) [[fallthrough]];
            ERAT_CHK210(9) [[fallthrough]]; ERAT_CHK210(10) [[fallthrough]]; ERAT_CHK210(11) [[fallthrough]];
            ERAT_CHK210(12) [[fallthrough]]; ERAT_CHK210(13) [[fallthrough]]; ERAT_CHK210(14) [[fallthrough]];
            ERAT_CHK210(15) [[fallthrough]]; ERAT_CHK210(16) [[fallthrough]]; ERAT_CHK210(17) [[fallthrough]];
            ERAT_CHK210(18) [[fallthrough]]; ERAT_CHK210(19) [[fallthrough]]; ERAT_CHK210(20) [[fallthrough]];
            ERAT_CHK210(21) [[fallthrough]]; ERAT_CHK210(22) [[fallthrough]]; ERAT_CHK210(23) [[fallthrough]];
            ERAT_CHK210(24) [[fallthrough]]; ERAT_CHK210(25) [[fallthrough]]; ERAT_CHK210(26) [[fallthrough]];
            ERAT_CHK210(27) [[fallthrough]]; ERAT_CHK210(28) [[fallthrough]]; ERAT_CHK210(29) [[fallthrough]];
            ERAT_CHK210(30) [[fallthrough]]; ERAT_CHK210(31) [[fallthrough]]; ERAT_CHK210(32) [[fallthrough]];
            ERAT_CHK210(33) [[fallthrough]]; ERAT_CHK210(34) [[fallthrough]]; ERAT_CHK210(35) [[fallthrough]];
            ERAT_CHK210(36) [[fallthrough]]; ERAT_CHK210(37) [[fallthrough]]; ERAT_CHK210(38) [[fallthrough]];
            ERAT_CHK210(39) [[fallthrough]]; ERAT_CHK210(40) [[fallthrough]]; ERAT_CHK210(41) [[fallthrough]];
            ERAT_CHK210(42) [[fallthrough]]; ERAT_CHK210(43) [[fallthrough]]; ERAT_CHK210(44) [[fallthrough]];
            ERAT_CHK210(45) [[fallthrough]]; ERAT_CHK210(46) [[fallthrough]]; ERAT_CHK210(47)
        }
    }
#undef ERAT_CHK210

done:
    i_io = i;
    w_io = w;
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
// Out of line (pinned, as measured): GCC inlines some classes into
// sieve_chunk on its own when surrounding code changes.
//
// State is split in two parallel arrays (struct of arrays): `dyn[i]` =
// (pos << 6) | w, rewritten every segment, and qp, read-only once
// activated. Rewriting qp along with pos/w every segment made the whole
// 8-byte state dirty -- ~1 MB per thread at 1e14, written back through L2
// and L3 every segment, flushing the segment itself: ~75% of this tier's
// L2 misses and ~80% of its L3 misses were on s[pos], not on the state.
// pos fits 26 bits (checked by SegmentSieve's constructor).
//
// The read-only qp is stored as a 1-byte delta from the previous prime of
// the same class (`qds[i]`; the list is sorted by p and never reordered,
// the first entry's delta is 0 from `qp_base`): consecutive primes of one
// class mod 30 are at most 52 * 30 apart below sqrt(1e15), so a byte holds
// it (checked at activation). 5 bytes per prime per segment instead of 8.
//
// NTA: prefetchnta both streams MEDIUM_NTA_DIST entries ahead, once per
// prime (not per hit), so they come into L1 without being allocated in L2
// and the segment stays there. Only pays once the medium state no longer
// fits the per-thread L3 share (dev PC: +0.8% at 1e12 with ~0.5 MB/thread,
// -6.1% at 1e13 and -12.1% at 1e14 on top of SoA) -- chosen per TierSet in
// main.cpp, see MEDIUM_NTA_MIN_PRIMES.
constexpr uint64_t MEDIUM_NTA_DIST = 32;
constexpr uint64_t MEDIUM_POS_LIMIT = uint64_t{1} << 26;

template <int PR, bool NTA>
__attribute__((noinline)) void cross_off_medium(uint8_t* s, uint64_t end, uint32_t* dyn, uint32_t* dyn_last,
                                                const uint8_t* qds, uint64_t qp_base, uint64_t rebase) {
    const uint32_t* pack = big::PACK210[PR].data();
    uint64_t qp = qp_base;
    for (; dyn != dyn_last; ++dyn, ++qds) {
        if constexpr (NTA) {
            __builtin_prefetch(dyn + MEDIUM_NTA_DIST, 0, 0);
            __builtin_prefetch(qds + MEDIUM_NTA_DIST * 4, 0, 0);
        }
        uint32_t d = *dyn;
        uint64_t pos = d >> 6;
        uint64_t w = d & 63;
        qp += *qds;
        while (pos < end) {
            uint32_t t = pack[w]; // mask | dm << 8 | corr << 16
            s[pos] |= static_cast<uint8_t>(t);
            pos += qp * ((t >> 8) & 0xff) + (t >> 16);
            if (++w == 96) [[unlikely]] w = 48;
        }
        if (w >= 48) w -= 48;
        *dyn = static_cast<uint32_t>(((pos - rebase) << 6) | w);
    }
}

// A 2-ahead software-prefetch variant of this loop, and an EratMedium-style
// 64-list restructuring keyed by (class, entry phase), were both tried and
// reverted -- real wins at some N that failed to hold up (or reversed
// outright) at this project's actual E13-E14 target range. See
// docs/RESEARCH.md for the full measurements.

// Medium tier in bands of a fixed, predicated iteration count
// (-DERA_MED_BANDS=1; off by default). The plain loop above leaves at a
// data-dependent `pos < end`, one mispredict per prime per segment; the
// i5-13500's Topdown puts ~90% of its remaining 2-thread gap in bad
// speculation. A class's list is sorted by p, so expected hits per segment
// (~6.857 * bytes / p) fall along it; activate_medium cuts it into bands
// with the same h = expected hits x factor + 1, and every prime of a band
// runs exactly h iterations with arithmetic masks (a hit past `end` marks
// the spare byte s[end] and doesn't advance; ?: would be compiled back into
// a branch). The trip count is constant across a band, so the exit is
// predicted; the plain loop afterwards catches the rare prime with more
// hits (correctness) and the primes expected above MEDIUM_BAND_MAX_HITS
// (h = 0). On the i5-11400F this loses (branch misses -50%, cycles +1.5..11%:
// the mispredict was overlapped there); see docs/RESEARCH.md.
struct MedBand {
    uint32_t end; // index one past the band's last prime in the class's list
    uint8_t h;    // fixed iterations, 0 = plain loop
};

template <int PR, bool NTA>
__attribute__((noinline)) void cross_off_medium_banded(uint8_t* s, uint64_t end, uint32_t* dyn, const uint8_t* qds,
                                                       uint64_t qp_base, uint64_t rebase,
                                                       const MedBand* band, const MedBand* band_last) {
    const uint32_t* pack = big::PACK210[PR].data();
    uint64_t qp = qp_base;
    uint32_t i = 0;
    for (; band != band_last; ++band) {
        const uint32_t bend = band->end;
        const unsigned H = band->h;
        for (; i < bend; ++i) {
            if constexpr (NTA) {
                __builtin_prefetch(dyn + i + MEDIUM_NTA_DIST, 0, 0);
                __builtin_prefetch(qds + i + MEDIUM_NTA_DIST * 4, 0, 0);
            }
            uint32_t d = dyn[i];
            uint64_t pos = d >> 6;
            uint64_t w = d & 63;
            qp += qds[i];
            for (unsigned h = 0; h < H; ++h) {
                const uint32_t t = pack[w];
                const uint64_t m = 0 - static_cast<uint64_t>(pos < end); // all ones on a hit
                s[(pos & m) | (end & ~m)] |= static_cast<uint8_t>(t);
                const uint64_t step = qp * ((t >> 8) & 0xff) + (t >> 16);
                pos += step & m;
                w += m & 1;
            }
            while (pos < end) {
                uint32_t t = pack[w];
                s[pos] |= static_cast<uint8_t>(t);
                pos += qp * ((t >> 8) & 0xff) + (t >> 16);
                if (++w == 96) [[unlikely]] w = 48;
            }
            while (w >= 48) w -= 48;
            dyn[i] = static_cast<uint32_t>(((pos - rebase) << 6) | w);
        }
    }
}

} // namespace erat
