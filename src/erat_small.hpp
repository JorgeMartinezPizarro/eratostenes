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

// Medium tier's mod-210 packing (see cross_off_medium below) needs 9 bits
// for the (residue class, mod-210 phase) index instead of the small
// tier's 6 (3 for pr + 3 for j), so qp only gets 23 bits here, not 26.
// Checked against the actual max medium-tier prime (bounded by segment
// width, not by base_prime_max) in SegmentSieve's constructor.
constexpr uint64_t QP_LIMIT_MEDIUM210 = uint64_t{1} << 23;

// Crosses off p = 30*qp + R[PR] in s[0, end), starting at the pending hit
// (i, j); on return (i, j) is the first hit at or past `end`.
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
// Mod-210 multiplier stepping (2026-09-24): every medium-tier prime is
// always > 163 (presieve's {7,23,37} group, presieve.hpp, always covers 7
// first), so any hit whose multiplier is a multiple of 7 is redundant --
// already marked composite by 7's own presieve pattern. Stepping through
// only the 48/210 multiplier phases coprime to 210 instead of the 8/30
// coprime to 30 (GAP_K210/ONFLY_CORRECTION210, wheel210_big.hpp -- same
// derivation as ONFLY_CORRECTION/GAP_K in wheel.hpp, just with M210
// standing in for WHEEL_R) skips ~14% of candidate hits in this tier. This
// is the same trick already validated and kept for the sparse tier's own
// big-wheel table (see main README/git history, "try big wheel for big
// primes"). Unlike the reverted 64-list restructuring below, this does
// NOT touch DenseState's layout or split the medium tier's single flat
// list -- only the shared constant tables grew a little (64 entries ->
// 48+8*48, still ~1.7KB, still trivially L1-resident) -- so there's no
// cache-vs-instructions trade being made here.
//
// A first version indexed a single combined table by a "next" field
// loaded from the previous lookup (mirroring the sparse tier's own
// big::TABLE) and measured a cycles:u REGRESSION despite real instruction
// savings -- the load-to-use chain through that field serializes one
// table load behind the previous one every hit. See wheel210_big.hpp's
// comment on GAP_K210/ONFLY_CORRECTION210 for the numbers and the fix
// (plain register-arithmetic index, like this file's own `j`/`w`).
//
// Measured after that fix (dev PC, i5-11400F, perf stat cycles:u, single
// run at a time, natural auto -s):
//   N=1e11: 121.118G -> 117.970G cycles:u (-2.6%), cache-refs 1.803B ->
//     1.825B (flat), cache-misses 12.52M -> 10.55M (-15.7%).
//   N=1e12: 1.5105T -> 1.4751T cycles:u (-2.3%), cache-refs 25.38B ->
//     20.40B (-19.6%), cache-misses 450.6M -> 422.8M (-6.2%).
//   N=1e13: 19.801T -> 19.776T cycles:u (-0.13%, noise-level) -- NOT the
//     growing win the original prediction here expected. Root cause: a
//     same-day, separate experiment (main.cpp, "EXPERIMENT IN PROGRESS")
//     shrinks seg_k_width to the nearest power of 2 once isqrt(limit)
//     reaches it, which happens by N=1e13 on this machine (2687 small /
//     152886 medium / 72036 sparse, vs 0 sparse at 1e12) -- that shift
//     moves a growing share of large medium-tier primes into the sparse
//     tier instead, so the medium tier's own population doesn't keep
//     growing with N here the way the (now-stale) reasoning in the
//     reverted 64-list writeup below assumed. Kept anyway: never measured
//     worse than flat at any N tried, no memory/layout cost paid, and a
//     real win at the N most runs actually spend most of their time at.
//     If the sparse/medium split changes again (segment-width tuning,
//     hardware), re-measure at 1e13+ before assuming this still helps
//     there.
//
// Attempt (tried, reverted): a 4-way interleaved version -- each prime's
// own chain (k -> next k) is a serial dependency, but four DIFFERENT
// primes' chains are independent, so the idea was to give out-of-order
// execution other ready work while one lane stalls on a branch-
// misprediction recovery or dependent load, instead of stalling through
// each prime fully before starting the next (the actual bit-set is a
// strided-free scatter, so there was nothing for the compiler to
// auto-vectorize the way presieve.hpp's fill() does -- this was meant as
// latency-hiding via interleaving, not SIMD; AVX-512 gather/scatter was
// ruled out up front too, since the target server, i5-13500/Raptor Lake,
// has AVX-512 fused off for having E-cores, unlike this dev PC). Measured
// with perf stat cycles:u (not wall-clock): +13.2% at N=1e11 (141.2G ->
// 159.8G), +14.3% at N=1e12 (1639.1G -> 1873.2G). IPC went up both times
// (1.37->1.52, 1.41->1.61) but instruction count rose even more
// (+25.6%/+30.8%) and branch-miss rate barely moved (7.97%->7.75%,
// 6.95%->6.45%) -- the hypothesized latency-hiding either didn't happen
// or didn't matter, while the real, measured cost was structural: the
// inner while loop runs until the LAST of the 4 lanes finishes, so a
// lane with fewer hits this segment still pays an `if (aN)` check every
// remaining iteration instead of retiring early like the scalar version's
// single while does per prime. Reverted.
// qw here packs (qp << 9) | (ri*48 + w) -- a different layout from the
// small tier's DenseState (qw = (qp<<6)|(pr<<3)|j) above; activate_dense
// (segment_sieve.hpp) packs medium entries this way from the start, never
// mixed with small-tier state. ri is split out once per prime (it's fixed
// for the whole call, like the old code's `pr`); w is a loop-carried
// index updated by plain increment-and-wrap, NOT by loading a "next"
// field out of the table -- see wheel210_big.hpp's comment on
// GAP_K210/ONFLY_CORRECTION210 for why that distinction is the whole
// difference between a win and a regression here.
inline void cross_off_medium(uint64_t* words, uint64_t end_bit, DenseState* first, DenseState* last, uint64_t rebase_bits) {
    for (DenseState* st = first; st != last; ++st) {
        uint64_t k = st->pos;
        uint64_t qp = st->qw >> 9;
        uint32_t idx = st->qw & 511;
        uint32_t ri = idx / 48;
        uint32_t w = idx % 48;
        const uint32_t* gap_k = big::GAP_K210.data();
        const uint32_t* corr = big::ONFLY_CORRECTION210[ri].data();
        while (k < end_bit) {
            words[k >> 6] |= uint64_t{1} << (k & 63);
            k += qp * gap_k[w] + corr[w];
            w = (w + 1 == 48) ? 0 : w + 1;
        }
        st->qw = static_cast<uint32_t>((qp << 9) | (ri * 48 + w));
        st->pos = static_cast<uint32_t>(k - rebase_bits);
    }
}

// Attempt (tried, reverted): "EratMedium"-style 64-list restructuring --
// reuse cross_off<PR> above (byte marking, constant masks) instead of this
// on-the-fly bit loop, split into WHEEL_SIZE*WHEEL_SIZE lists keyed by
// (class PR, entry phase J) instead of one flat list, so switch(j) above
// becomes a compile-time-constant jump per list rather than a runtime
// dispatch. Idea from an external review (Opus 5.5, 2-vCPU VM, no perf
// access) predicting this would help MORE as N grows, since the medium
// tier's share of total cycles grows with N.
//
// Measured (dev PC, i5-11400F, perf stat cycles:u, natural auto -s):
//   N=1e12: cycles:u 1.509T -> 1.335T (-11.5%), instructions:u -39.8%
//     (matching the ~2-instructions-per-hit claim above), cache-miss rate
//     1.97%->5.16%, IPC 1.32->0.90.
//   N=1e13: cycles:u 20.671T -> 19.582T (-5.3%), instructions:u -32.7%,
//     cache-miss rate 5.57%->12.04%, IPC 1.36->0.97.
// A real, reproducible win at both N (independently re-verified: 1e12
// reproduced at -11.2% cycles:u, cache-miss 2.19%->5.09%, near-exact match)
// -- but the OPPOSITE trend from the one predicted: the win roughly halves
// from 1e12 to 1e13 while the cache-miss rate roughly doubles, because 64
// lists (vs one flat array) cost extra memory footprint/locality that grows
// with the medium-tier population -- the same failure mode as the sparse
// tier's own attempt 3 (segment_sieve.hpp): trading instructions for cache
// misses, on a codebase whose actual wins so far have all come from the
// opposite trade (reducing cache misses, see L1 sub-block decoupling and
// the segment-width fix). Instruction savings stayed roughly flat (-39.8%
// -> -32.7%) while the miss-rate cost roughly doubled -- extrapolating that
// divergence past 1e13 toward this project's actual E14/E15 target range
// (see project roadmap), the miss-rate cost plausibly overtakes the
// instruction savings and flips this from a win to a regression well before
// reaching the N that matters here. Reverted for that reason -- not because
// it measured as a loss at the N actually tested, but because the trend
// argues against it holding up at the N this project targets. If ever
// revisited, re-derive at N=1e14+ first rather than trusting the 1e12/1e13
// trend to extrapolate favorably.

} // namespace erat
