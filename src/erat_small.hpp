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
// Tried, reverted (2026-09-25): every local here (qp, p, the o[j]s, b,
// end) is genuinely bounded well under 2^20 regardless of N -- qp by
// small_limit (an L1-cache-derived constant, never N-dependent), the
// rest by the segment's own byte width (seg_k_width/8, capped under 2^30
// by SegmentSieve's own constructor check) -- so narrowing every local
// from uint64_t to uint32_t looked like a free win (shorter x86-64
// encoding, no REX prefix) with no range risk. Measured the opposite:
// dev PC, i5-11400F, perf stat cycles:u, natural auto -s, two reps each --
// N=1e11: 118.44G/118.50G -> 120.19G/120.24G cycles:u (+1.47% both reps);
// N=1e12: 1.4604T/1.4645T -> 1.4742T/1.4805T cycles:u (+0.95%/+1.09%).
// instructions:u rose too (+3.2% at 1e11, +2.5% at 1e12, identically
// across reps -- deterministic, not noise), the opposite of the
// instruction-count savings the shorter encoding was expected to give.
// Root cause not isolated further (would need perf annotate to see
// exactly which instructions the compiler added), but the practical
// takeaway holds regardless: on this compiler/target, 64-bit locals for
// pointer-offset arithmetic on x86-64 apparently let GCC's optimizer do
// something it can't when 32/64-bit types mix, even though every value
// involved provably fits in 32 bits. Reverted to uint64_t throughout.
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
// Considered, not implemented (2026-09-25, external review, Opus 5.5):
// the obvious next step past mod-210 is mod-2310 (skip 7, 11 AND 13's
// redundant multiples, all three already covered by PRESIEVE_GROUPS) --
// 480 phases instead of 48, ~9% fewer candidate hits in both this tier
// and the sparse one. Checked the actual table cost before writing any
// code: wheel210_big.hpp's Entry is 8 bytes; the sparse tier's flat
// per-(class,phase) table would grow from 384 entries (3KB) to 8*480=3840
// (30KB) -- not the review's own ~15KB estimate, which this file's git
// history has no matching derivation for; this tier's own idx already
// needs 12 bits instead of 9 to address it. This file's two tables
// (GAP_K210 + ONFLY_CORRECTION210) would grow from ~1.7KB to ~17.3KB.
// Both land at or past the 32KiB L1d the review itself flags on the
// server's E-cores -- and that's each table ALONE, before counting
// whatever else (DenseState arrays, the segment bit array) needs L1 at
// the same time. Same conclusion wheel.hpp already reached for the mod-
// 2310 BASE wheel, for the same underlying reason (a wheel's table cost
// grows with the product of its primes; the candidate reduction only
// grows with their sum of reciprocals) -- this is that argument applying
// a second time, one level down, to the stepping tables instead of the
// whole-program layout. Not implemented: the review's own estimate was a
// modest 1-3% gain, likely optimistic given the corrected table sizes,
// against a real risk of blowing L1 on the actual target hardware.
//
// A second, independent reason kills the SPARSE tier's half of this
// outright, past just cache pressure: DenseState.qw is a uint32_t, and
// mod-210 there already spends 9 bits on (class, phase) (8*48=384,
// needs 9), leaving 23 for qp -- max representable prime ~251.66M
// (qp_max*30), comfortably past isqrt(1e15)~31.62M, this project's own
// declared E15 target (see MEMORY.md/project scope), with ~8x headroom.
// Mod-2310 needs 12 bits for (class, phase) (8*480=3840), leaving only
// 20 for qp -- max representable prime ~31.46M, which is BELOW
// isqrt(1e15). Past that point qp silently wraps and the sieve produces
// wrong results with no error -- this isn't a performance tradeoff
// against a modest gain any more, it's incompatible with a goal this
// project has already committed to, short of a bigger restructuring of
// DenseState's packing than this idea was ever meant to be. (Aside,
// found while checking this: base_prime_max -- which is what the sparse
// tier's own qp actually has to fit, not seg_k_width -- has no runtime
// assertion today, under the CURRENT mod-210 packing either; harmless at
// E15 given the 8x headroom above, but worth a real check if this
// project's own target ever moves past roughly limit=6.3e16.) The
// MEDIUM tier's mod-2310 half doesn't have this problem (its primes stay
// under seg_k_width, orders of magnitude below this ceiling either way)
// -- it's still just the cache-pressure argument above for that tier,
// not a hard rejection.
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
// Class-specialized (2026-09-24, matching primesieve's own EratMedium
// split into crossOff_7/11/13/.../31, one per residue class): PR is now a
// compile-time template parameter, one list per class (medium_[8] in
// segment_sieve.hpp, same shape as the small tier's small_[8]) instead of
// one flat list carrying a runtime `ri`. big::ONFLY_CORRECTION210[PR] is
// now a compile-time-constant row offset (foldable into the load's
// displacement) instead of a per-prime runtime-computed pointer
// (ONFLY_CORRECTION210[ri].data()) -- removes one multiply-by-row-size
// per prime per segment (not per hit; the real per-hit cost, qp*gap_k[w],
// is unavoidable here -- primesieve's own EratMedium avoids it by
// precomputing distinct per-prime deltas, but that's sized for its 8-phase
// mod-30 wheel; with 48 phases here, precomputing all of them costs more
// than the 1-3 hits/segment typical of this tier would recoup -- see
// wheel210_big.hpp's comment for the numbers behind that tradeoff).
//
// qw packs (qp << 6) | w now (w alone needs 6 bits, 0..47, same budget as
// the small tier's (pr<<3)|j) -- back to the small tier's own QP_LIMIT
// budget (2^26), not the tighter QP_LIMIT_MEDIUM210 the old (qp<<9)|
// (ri*48+w) packing needed.
//
// This DOES split medium_ into 8 lists, the same shape as the reverted
// 64-list attempt that lost to cache-footprint growth -- but 8 lists is a
// much smaller fragmentation than 64, and each still holds every prime of
// its OWN class permanently (no per-segment migration between lists, since
// a prime's class never changes) -- structurally identical to how small_[8]
// already works without issue. Measure at 1e12 AND 1e13 before trusting
// either alone (this tier's population saturates at its permanent max
// once sqrt(N) exceeds seg_k_width, somewhere between those two N on this
// machine -- see git history/session notes for the exact threshold), since
// this tier's own history has already produced opposite-signed results at
// those two N more than once.
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
//
// CONFIRMED (2026-09-25, follow-up session, fresh implementation -- the
// original above was never committed, so this was rewritten from scratch,
// not recovered): re-attempted this exact idea, now with real perf access
// on this dev PC (see project memory on the sudo/perf permission story)
// instead of trusting the trend extrapolation above. New double-buffered
// design (medium64_cur_/medium64_next_ in segment_sieve.hpp, swapped each
// segment instead of migrating in place) to sidestep any implementation-
// specific confound. Measured (perf stat cycles:u, single clean run each,
// natural auto -s):
//   N=1e12: cycles:u 1.4644T -> 1.3791T (-5.8%), instructions:u -32.1%,
//     cache-miss rate (LLC) 4.91%, IPC 1.26->0.91. A real win, smaller
//     than the original writeup's -11.5% but the same direction.
//   N=1e13: cycles:u 19.412T -> 20.042T (+3.25%, a REGRESSION, not just a
//     smaller win), instructions:u still -18.9% (real, substantial), but
//     cache-misses:u nearly DOUBLED (20.06B -> 39.29B, +95.8%) and fully
//     consumed the instruction savings.
// This directly confirms the trend-based rejection above was correct --
// not by extrapolating from two points this time, but by measuring the
// actual N=1e13 regression directly. The idea is now closed on real data
// at the N this project targets (E13-E14), not just a projection past it.
// Don't re-attempt without a fundamentally different fix for the
// footprint-vs-instruction trade (e.g. shrinking DenseState itself, or
// bounding how many of the 64 lists can be simultaneously "hot") -- the
// trade direction itself (fewer instructions for more cache pressure) has
// now failed this same trend check three times in this codebase (see also
// segment_sieve.hpp's sparse-tier attempt 3, and the sparse_limit/4 cutoff
// experiment in main.cpp).

} // namespace erat
