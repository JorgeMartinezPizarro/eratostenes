#pragma once
// Byte-addressed crossing-off kernels for the dense tiers (small, med64,
// medium) -- primesieve's EratSmall/EratMedium ideas, re-derived for this
// layout.
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
// table load, no variable shift -- about 2 instructions per hit.

#include <algorithm>
#include <cstdint>

#include "wheel.hpp"
#include "wheel210_big.hpp"

namespace erat {

// The mod-30 residues; C and M give the byte offset and bit mask of the
// hit of class pr at multiplier phase j (see above).
inline constexpr const auto& R = WHEEL_R;
constexpr uint64_t C(int pr, int j) { return R[pr] * R[j] / 30; }
constexpr uint8_t M(int pr, int j) { return static_cast<uint8_t>(1u << WHEEL_POS[R[pr] * R[j] % 30]); }

// Per-prime state of the small and med64 tiers, and the sparse tier's 8-byte
// entry slot: qw = (qp << 6) | (pr << 3) | j (small) or (qp << 6) | w (med64),
// with qp = p / 30, pr the residue class and j / w the pending hit's
// multiplier phase. pos, relative to the current segment's first byte: for
// med64 the pending hit's byte position; for the small tier the base of
// that hit's multiplier cycle (see cross_off), a signed value stored as its
// low 32 bits. qp < 2^26 (p < ~2e9) is checked by SegmentSieve's constructor.
struct DenseState {
    uint32_t qw;
    uint32_t pos;
};

constexpr uint64_t QP_LIMIT = uint64_t{1} << 26;

// Byte offset of the phase-j hit from its cycle base, for a prime of class
// pr with qp = p / 30 (the o_j of cross_off, at run time).
inline uint64_t small_offset(uint64_t qp, uint64_t pr, uint64_t j) {
    return qp * (R[j] - 1) + R[pr] * R[j] / 30;
}

// Small-tier state for p's first hit at or past start_val (the smallest
// multiplier coprime with 30 there): its phase j, and the base of its cycle
// relative to byte k_low / 8 (the hit's byte minus small_offset; may be
// negative). Its residue class is (qw >> 3) & 7.
inline DenseState small_state(uint64_t p, uint64_t start_val, uint64_t k_low) {
    const uint64_t pr = static_cast<uint64_t>(WHEEL_POS[p % WHEEL_MOD]);
    uint64_t m = (start_val + p - 1) / p;
    uint64_t r = m % WHEEL_MOD;
    const uint64_t step = STEP_TO_COPRIME[r];
    m += step;
    r += step;
    if (r >= WHEEL_MOD) r -= WHEEL_MOD;
    const uint64_t qp = p / WHEEL_MOD;
    const uint64_t j = static_cast<uint64_t>(WHEEL_POS[r]);
    const uint64_t pos = (p * m) / WHEEL_MOD - k_low / 8;
    return {static_cast<uint32_t>((qp << 6) | (pr << 3) | j), static_cast<uint32_t>(pos - small_offset(qp, pr, j))};
}

// cross_off's result, returned in two registers: the cycle base b and the
// phase j of the first hit at or past `end`.
struct SmallPending {
    uint64_t b;
    uint64_t j;
};

// Small tier: crosses off p = 30*qp + R[PR] in s[0, end), from the pending
// hit at phase j of the cycle based at byte b; returns the first hit at or
// past `end` the same way. The phase-j hit is at b + o_j, o_j = qp*(R[j]-1) +
// C(PR, j), and R[j] - 1 = 0, 6, 10, 12, 16, 18, 22, 28 for every class: three
// bases moving by p per cycle (q = s + b, q + 6qp, q + 10qp) and two fixed
// indices (6qp, 12qp) reach all 8 hits as base + index + constant, one `or`
// each -- the address registers GCC's own strength reduction picks for this
// loop, written out so the entry can share them. When the rest of the entry
// cycle lies before `end` (the usual case: p is a fraction of the
// sub-block), the switch enters the unrolled loop itself at phase j (Duff's
// device); otherwise a checked chain returns within that cycle. After the
// loop a checked chain leaves at `end`.
//
// The state is the cycle base, not the hit's byte, so no o_j is ever looked
// up by phase; b may be "negative" (wrapped) when the pending hit is early in
// its cycle, and is only ever used in sums, exact mod 2^64. Inlined into
// cross_off_class's loop over a class's primes: no call per prime. Most small
// primes cross a sub-block in a few cycles, so the entry and exit are a large
// share of the tier: docs/RESEARCH.md#small-tier-the-cycle-base-as-state-the-kernel-inlined-a-check-free-entry-cycle-kept-2026-11-16.
template <int PR>
__attribute__((always_inline)) inline SmallPending cross_off(uint8_t* s, uint64_t end, uint64_t qp, uint64_t b, uint64_t j) {
    const uint64_t p = 30 * qp + R[PR];
    const uint64_t q6 = qp * 6, q12 = qp * 12;
    const uint64_t o7 = qp * 28 + C(PR, 7);
    const uintptr_t sa = reinterpret_cast<uintptr_t>(s);
    const uintptr_t se = sa + end;
    uintptr_t q = sa + b, a6 = q + q6, a10 = q + qp * 10;
#define ERAT_A0 (q + C(PR, 0))
#define ERAT_A1 (a6 + C(PR, 1))
#define ERAT_A2 (a10 + C(PR, 2))
#define ERAT_A3 (a6 + q6 + C(PR, 3))
#define ERAT_A4 (a10 + q6 + C(PR, 4))
#define ERAT_A5 (a6 + q12 + C(PR, 5))
#define ERAT_A6 (a10 + q12 + C(PR, 6))
#define ERAT_A7 (q + o7)
#define ERAT_MARK(J) *reinterpret_cast<uint8_t*>(ERAT_A##J) |= M(PR, J)
#define ERAT_CHK(J) \
    if (ERAT_A##J >= se) return {q - sa, J}; \
    ERAT_MARK(J);

    // q < lim: hit 7 of the cycle based at q is before `end`. No wrap: s is
    // far from 0 and every base within a cycle of the range.
    const uintptr_t lim = se - o7;
    if (q < lim) [[likely]] {
        switch (j) {
            default: __builtin_unreachable();
            for (;;) {
                case 0: ERAT_MARK(0); [[fallthrough]];
                case 1: ERAT_MARK(1); [[fallthrough]];
                case 2: ERAT_MARK(2); [[fallthrough]];
                case 3: ERAT_MARK(3); [[fallthrough]];
                case 4: ERAT_MARK(4); [[fallthrough]];
                case 5: ERAT_MARK(5); [[fallthrough]];
                case 6: ERAT_MARK(6); [[fallthrough]];
                case 7: ERAT_MARK(7);
                q += p;
                a6 += p;
                a10 += p;
                if (q >= lim) break;
            }
        }
    } else {
        // Hit 7 is past `end`, so this chain returns before its end.
        switch (j) {
            case 0: ERAT_CHK(0) [[fallthrough]];
            case 1: ERAT_CHK(1) [[fallthrough]];
            case 2: ERAT_CHK(2) [[fallthrough]];
            case 3: ERAT_CHK(3) [[fallthrough]];
            case 4: ERAT_CHK(4) [[fallthrough]];
            case 5: ERAT_CHK(5) [[fallthrough]];
            case 6: ERAT_CHK(6) [[fallthrough]];
            case 7: return {q - sa, 7};
            default: __builtin_unreachable();
        }
    }
    ERAT_CHK(0)
    ERAT_CHK(1)
    ERAT_CHK(2)
    ERAT_CHK(3)
    ERAT_CHK(4)
    ERAT_CHK(5)
    ERAT_CHK(6)
#undef ERAT_CHK
#undef ERAT_MARK
#undef ERAT_A0
#undef ERAT_A1
#undef ERAT_A2
#undef ERAT_A3
#undef ERAT_A4
#undef ERAT_A5
#undef ERAT_A6
#undef ERAT_A7
    return {q - sa, 7}; // hit 7 is past `end` from the loop exit
}

// med64 tier: same contract as cross_off, EratMedium's loop shape -- a
// switch into a for (;;) with one running byte index and one bounds check
// per hit, so a call leaves at a single loop exit. Steps on the mod-210
// multiplier wheel (w = 0..47, M210[w]): multipliers divisible by 7 are
// skipped, 7 being presieved. The byte step from phase w to w+1 is
// qp*dm + corr with dm in {2,4,6,8,10}: five multiples of qp in registers
// plus compile-time constants from big::TABLE cover all 48 cases. See
// docs/RESEARCH.md#med64-mod-210-stepping-on-the-checked-loop-cross_off_checked210-kept-2026-09-30.
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
// s[0, end), then rebases each pending cycle base by `rebase` bytes (the
// segment's byte width on its last pass over a segment, 0 otherwise). One
// list per class (see SegmentSieve::small_) so the class dispatch happens
// once per list, not as an unpredictable switch per prime. Out of line,
// with cross_off inlined into it.
template <int PR>
__attribute__((noinline)) void cross_off_class(uint8_t* s, uint64_t end, DenseState* first, DenseState* last, uint64_t rebase) {
    for (DenseState* st = first; st != last; ++st) {
        const uint64_t qw = st->qw;
        // The base is signed (small_state): sign-extended from its 32 bits.
        const auto b = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(st->pos)));
        const SmallPending r = cross_off<PR>(s, end, qw >> 6, b, qw & 7);
        st->qw = static_cast<uint32_t>((qw & ~uint64_t{7}) | r.j);
        st->pos = static_cast<uint32_t>(r.b - rebase);
    }
}

// Medium tier: primes with a few hits per segment, too few to pay the
// small tier's entry/exit per call, so one hit per iteration through a
// table: s[pos] |= mask, then pos += qp * dm + corr from PACK210[PR][w]
// (wheel210_big.hpp; mod-210 multipliers, multiples of 7 skipped). One list
// per residue class keeps PR a template parameter (EratMedium's
// crossOff_7/11/.../31). The table holds two 48-phase cycles, so w only
// wraps at 96 -- once per call at most -- and is folded back after the loop.
//
// State is a struct of arrays: dyn[i] = (pos << 6) | w, rewritten every
// segment, and qp, read-only, as a 1-byte delta from the class's previous
// prime (qds[i]; the list is sorted by p and never reordered; medium primes
// are below the segment width, where same-class gaps stay far under 255 * 30,
// checked at activation). Only the dyn half is ever dirty: 5 bytes stream
// per prime per segment. pos fits 26 bits (SegmentSieve's constructor).
//
// NTA: both streams prefetched MEDIUM_NTA_DIST entries ahead with
// prefetchnta, once per prime, so they reach L1 without being kept in L2;
// on only once the state outgrows the per-thread L3 share (tuning.hpp's
// medium_nta_min_primes). See
// docs/RESEARCH.md#cross_off_medium-struct-of-arrays-state--gated-prefetchnta-kept-2026-09-29.
//
// The tier is bound by its loop exit per prime, not by its hits
// (RESEARCH.md's `cross_off_medium` entries). Out of line (pinned): GCC
// inlines some classes into sieve_chunk on its own otherwise.
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

} // namespace erat
