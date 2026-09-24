#pragma once
// Segmented sieve on a compile-time wheel (see wheel.hpp), bit-packed into
// uint64_t words.
//
// Three prime tiers, chosen by expected hits (see main.cpp for the actual
// cutoffs) -- this mirrors primesieve's own split (EratSmall / EratMedium /
// EratBig): a prime p's average gap between hits, in wheel-index terms, is
// ~p (each wheel-coprime multiplier step advances the value by
// ~p*WHEEL_MOD/WHEEL_SIZE, which converts back to a k-gap of ~p).
//
//   - small_primes (p < L1d/2 bytes, >= ~16 hits per L1-sized sub-block):
//     FLAT, crossed off one L1-sized sub-block at a time (presieve fill
//     first, then every small prime), so the marks -- ~80%+ of all of them
//     -- land in L1 instead of the L2-sized segment. Uses erat_small.hpp's
//     unrolled 8-hits-per-cycle loop with compile-time bit masks, one list
//     per residue class p % 30 (small_[pr]) so the class dispatch isn't an
//     unpredictable branch per prime.
//   - medium_primes (up to the segment width, so >= ~1 hit/segment): FLAT,
//     one pass over the whole segment, generic one-hit-per-iteration
//     stepping via ONFLY_CORRECTION (wheel.hpp: one multiply by qp=p/30,
//     one shared-table lookup, one add). The unrolled loop was measured
//     slower here: with only a few hits per segment it can't amortize its
//     unpredictable entry/exit (see erat_small.hpp::cross_off_medium). An
//     EratMedium-style 64-list restructuring (byte marking like the small
//     tier, keyed by (class, entry phase) so the entry point is a
//     compile-time constant per list) was tried and reverted -- real win at
//     N=1e12/1e13 tested but on a trend (cache-miss cost growing faster
//     than the instruction savings) that argues against it holding up at
//     this project's actual E14/E15 target range; see the full writeup on
//     erat_small.hpp::cross_off_medium.
//     Both flat tiers keep 8 bytes/prime of state (erat::DenseState),
//     walked every segment in place -- there is never a segment these
//     primes "skip", so a bucket would buy nothing.
//
//     These two tiers replaced (dev PC, i5-11400F, cycles:u) a per-prime
//     delta[] table tier (40 bytes/prime, capped by an L3/2 budget) plus
//     the ONFLY_CORRECTION loop for everything past that budget, both on
//     the whole L2-sized segment: -26% at N=1e11, -30% at N=1e12.
//   - sparse_primes (p >= segment width, at most ~1 hit/segment): BUCKET.
//
//     EXPERIMENT IN PROGRESS (isolated test of point 1 from an external
//     review, Opus 5.5, 2026-09-24): the ONFLY_CORRECTION-stepping tier
//     documented below (attempts 1-7) has been swapped for an EratBig-style
//     rewrite -- byte marking (not bit), a mod-210 multiplier wheel (48/210
//     phases instead of 8/30 -- valid because any multiplier that's a
//     multiple of 7 lands on a composite 7 itself already crosses off in
//     its own small-tier pass, so those phases are redundant work here
//     specifically, never a correctness gap), and pointer-aligned blocks
//     (a tail pointer landing exactly on a block boundary means "full",
//     primesieve's own Bucket trick, no per-block count field to load).
//     Requires a power-of-2 segment width in BYTES (see the constructor's
//     has_sparse check) for the bucket-slot math to become a shift/mask
//     instead of a division -- main.cpp floors seg_k_width to the nearest
//     power of 2 whenever this tier is used. NOTE: rounding the
//     auto-computed width down to a power of 2 was already tried in
//     isolation for the OLD tier (attempt 7 below) and did NOT give a
//     consistent win on this dev PC (cache-refs got 12.9% WORSE at N=1e13)
//     -- since that rounding is now a hard requirement of this new tier's
//     design, any win/loss measured here is the two effects bundled
//     together, not the EratBig rewrite in isolation. Being measured with
//     perf stat cycles:u at N=1e12/1e13 before deciding whether to keep
//     this over the documented-below original. The small/medium tiers are
//     untouched by this experiment.
//
//     Below this experiment note, the ORIGINAL tier's history (BUCKET
//     design rationale, attempts 1-7): most segments have nothing
//     to do for most of these primes, so scheduling each one into the
//     future segment where its next hit actually falls (a fixed-size
//     ring of block-pooled queues, see process_sparse_bucket's attempt-6
//     comment) means a segment's processing only ever looks at the (few)
//     sparse primes actually due, not all of them. Steps forward the same
//     qp*GAP_K[j] + ONFLY_CORRECTION[pr][j] way the medium tier does -- no
//     division by the runtime value p anywhere in this tier (see attempt 5
//     below) -- with qp/pr/the wheel phase j packed into one word
//     (erat::DenseState::qw, same layout as the dense tiers) alongside a
//     `pos` relative to whichever segment the entry is due in, 8 bytes
//     total per live entry.
//
//     Five changes to this tier's *stepping math* were tried and
//     reverted as net regressions -- two predate splitting
//     process_sparse_bucket (below) into its own noinline function (see
//     that split's own commit): a shared-table scheme like
//     ONFLY_CORRECTION above (~2x slower at N=1e12 with a third of base
//     primes forced sparse) and an AoS relayout of its per-prime state for
//     locality (~2.25x slower), both measured while this whole function
//     (dense + onfly + sparse) was still fully inlined and suffering real
//     register spilling -- any change adding live variables looked
//     catastrophic there regardless of its own merit.
//       - attempt 3 (dev PC session, N=1e13, natural auto -s, 72,036/
//         227,647 base primes sparse): retried the shared-table scheme
//         *after* the noinline split, on the theory that attempt 1's loss
//         was purely the register-spilling confound. It wasn't -- clean
//         same-session A/B via perf stat cycles:u (frequency-independent,
//         not wall-clock): 48.11T cycles vs 42.68T baseline, +12.7%; IPC
//         0.96->0.85; cache-miss rate 9.52%->12.55%. Root cause: this
//         scheme needs qp+pr per prime (OnFlyPrime, 24 bytes padded)
//         instead of the plain uint64_t p (8 bytes) sparse_primes held
//         before, nearly tripling that array's footprint right in the
//         tier accessed in pseudo-random order (via the bucket ring's
//         intrusive list, no locality to begin with) -- costs more in
//         cache pressure than the division (measured elsewhere as ~2.6%
//         of this tier's cycles) saves. Register spilling was real for
//         attempts 1-2, but wasn't the whole story either apparently --
//         or this tier's memory-footprint sensitivity is itself the
//         thing that changed between N=1e12 (attempts 1-2) and N=1e13
//         (attempt 3), given how much bigger the sparse population is at
//         the natural cliff vs a forced-small-N proxy. Reverted.
//       - attempt 4 (superseded by attempt 5 below -- dev PC, N=1e13,
//         natural auto -s, same 72,036/227,647 sparse split as attempt 3):
//         same division-free goal as attempt 3, but stores m instead of adding
//         qp/pr fields, so there's no memory-footprint growth to fight the
//         division's removal with -- wheel_index(p*m) (a multiply plus a
//         compile-time-constant divide) replaces both the old
//         wheel_number(k)/p division *and* the extra per-prime storage
//         attempt 3 needed. Clean same-session A/B via perf stat
//         cycles:u: 41.39T vs 42.68T baseline, -3.0%; wall-clock 1011.38s
//         vs 1094.00s, -7.55%; IPC 0.96->1.00; cache-miss rate flat
//         (9.52%->9.63%, confirming no footprint growth this time).
//       - attempt 5 (dev PC session, same day): a division-by-p removal
//         that fixes attempt 3's actual failure (memory footprint, not
//         the division itself) directly, instead of attempt 4's
//         alternative fix (wheel_index(p*m) instead of the shared-table
//         step). qp=p/WHEEL_MOD and pr=p%WHEEL_MOD are recomputed from p
//         per due-check rather than stored -- both are divisions by the
//         *compile-time* constant WHEEL_MOD, a cheap multiply-shift, not
//         the runtime-p division being removed -- and k+j are packed into
//         one word (sparse_kj_) instead of two, so total per-prime memory
//         stays exactly what attempt 4's plain k took. Measured two ways:
//         a forced-heavy-sparse proxy (N=1e12, -s 500000, 84% sparse, the
//         same trick used elsewhere in this file to test this tier
//         cheaply) showed a clean win -- cycles:u 4.480T->4.112T (-8.2%),
//         wall-clock 96.57s->89.06s (-7.8%), instructions:u down too (not
//         just cycles), cache-miss rate 4.04%->3.48%. At the *natural*
//         N=1e13 cliff (31.6% sparse, this tier only ~17% of total cycles
//         per a perf profile taken this session) the same fix only moved
//         wall-clock 898.63s->892.09s (-0.7%) -- small because the tier
//         itself is still a minority of the work at this N, not because
//         the fix doesn't hold up; instructions:u still dropped
//         (42.819e9->42.293e9), confirming a real if modest effect here,
//         expected to matter more as N grows past 1e13 and this tier's
//         share of total cycles grows with it (see main.cpp's comment on
//         the segment-width L2 cliff). Kept.
//
// Extraction (turning the finished bit array into actual prime values):
// invert each word, decompose into (q, r) = (k / WHEEL_SIZE, k %
// WHEEL_SIZE) once per word, then walk set bits with ctz + clear-lowest-bit.

#include <cstdint>
#include <vector>
#include <memory>
#include <cstdlib>
#include <algorithm>
#include <stdexcept>

#include "erat_small.hpp"
#include "presieve.hpp"
#include "wheel.hpp"
#include "wheel210_big.hpp"

class SegmentSieve {
public:
    // seg_k_width: wheel-index width of a normal (non-final) segment, same
    // as passed to sieve_and_emit's k_high-k_low in every call but the
    // last of a chunk.
    // base_prime_max: the largest base prime this sieve will ever be given
    // (i.e. isqrt(limit)) -- used to size the bucket ring generously
    // enough that no sparse prime's skip between hits can ever wrap around
    // it.
    // presieve: shared, read-only pre-sieve pattern (see presieve.hpp) used
    // to fill each segment instead of zeroing it; its primes must already
    // be excluded from small/medium/sparse primes by the caller.
    // sub_block_bytes: L1-sized slice the small tier (and presieve fill)
    // is run over, one slice at a time -- see sieve_and_emit.
    // has_sparse: true when this run actually has any sparse-tier primes.
    // The new EratBig-style tier below needs seg_k_width/8 (the segment
    // width in BYTES) to be a power of 2 so its bucket-slot math is a
    // shift/mask instead of a division -- main.cpp is responsible for
    // flooring seg_k_width to the nearest power of 2 (in bytes) whenever
    // has_sparse is true; this constructor just verifies that was done.
    SegmentSieve(uint64_t seg_k_width, uint64_t base_prime_max, const Presieve& presieve,
                 uint64_t sub_block_bytes, bool has_sparse)
        : words_((seg_k_width + 63) / 64, 0),
          seg_k_width_(seg_k_width),
          sub_block_bytes_(sub_block_bytes),
          presieve_(presieve) {
        // The byte-addressed dense tiers (erat_small.hpp) need every
        // segment to start on a byte (k multiple of 8) and to stay a whole
        // number of words; callers align chunk starts to 64 too.
        if (seg_k_width % 64 != 0 || sub_block_bytes % 8 != 0 || sub_block_bytes == 0) {
            throw std::runtime_error("SegmentSieve: ancho de segmento/sub-bloque no alineado");
        }
        // Dense primes are p < seg_k_width, so their pending hit stays
        // within a few segment widths of the segment start (fits
        // DenseState::pos), and p / 30 has to fit its packed qp field.
        if (seg_k_width / WHEEL_MOD >= erat::QP_LIMIT || seg_k_width >= (uint64_t{1} << 30)) {
            throw std::runtime_error("SegmentSieve: segmento demasiado grande para el estado denso empaquetado");
        }
        uint64_t sb = seg_k_width_ / 8; // segment width in bytes
        if (has_sparse && (sb & (sb - 1))) {
            throw std::runtime_error("SegmentSieve: el tier disperso (EratBig) necesita un segmento potencia de 2 (bytes)");
        }
        log2_sb_ = 0;
        while ((uint64_t{1} << log2_sb_) < sb) ++log2_sb_;
        // Largest BYTE step between one sparse prime's consecutive hits:
        // qp * max(dm) + max(corr), with max(dm) = 10 on the mod-210
        // multiplier wheel (the largest gap between consecutive 210-
        // coprime residues) -- a few extra WHEEL_SIZE's of slack (+16)
        // cost nothing (buckets are cheap) and keep this comfortably safe.
        uint64_t maxstep = base_prime_max / WHEEL_MOD * 10 + 16;
        uint64_t ahead = (maxstep >> log2_sb_) + 2;
        num_buckets_ = 1;
        while (num_buckets_ < ahead * 2) num_buckets_ <<= 1; // power of 2, 2x margin
        head_.assign(num_buckets_, nullptr);
        tail_.assign(num_buckets_, nullptr);
    }

    // Must be called once before the first sieve_and_emit call for a new,
    // independent run of consecutive segments in increasing k order (a
    // thread's chunk). Resets all bucket/flat-tier state and the "which
    // primes have activated yet" pointers. Never reuse a SegmentSieve
    // across threads or out of order.
    void begin_chunk() {
        cur_segment_ = 0;
        next_small_idx_ = 0;
        next_medium_idx_ = 0;
        next_sparse_idx_ = 0;
        for (auto& v : small_) v.clear();
        medium_.clear();
        std::fill(head_.begin(), head_.end(), nullptr);
        std::fill(tail_.begin(), tail_.end(), nullptr);
        // Blocks aren't freed, just handed back to the pool: every block
        // ever allocated for this SegmentSieve is reusable, so repopulate
        // the free list from scratch rather than reallocate.
        free_.clear();
        for (auto& c : chunks_)
            for (size_t i = 0; i < CHUNK_BLOCKS; ++i)
                free_.push_back(reinterpret_cast<Blk*>(c.get() + i * BLK_BYTES));
    }

    template <typename Writer>
    void sieve_and_emit(uint64_t k_low, uint64_t k_high,
                         const std::vector<uint64_t>& small_primes,
                         const std::vector<uint64_t>& medium_primes,
                         const std::vector<uint64_t>& sparse_primes,
                         Writer& out, uint64_t& prime_count) {
        uint64_t count = (k_high > k_low) ? (k_high - k_low) : 0;
        if (count == 0) return;

        size_t words_needed = (count + 63) / 64;
        uint64_t bytes_needed = (count + 7) / 8;

        uint64_t high_n = wheel_number(k_high); // exclusive numeric bound, valid for the p*p cutoff
        uint64_t low_n = wheel_number(k_low);

        // Activate any base primes that just became relevant (p*p < high_n).
        // Each of the three lists is sorted by p and gets its own
        // monotonically-advancing pointer, so every prime is visited here
        // exactly once for the whole chunk, not once per segment.
        //
        activate_dense(small_primes, next_small_idx_, small_, true, high_n, low_n, k_low);
        activate_dense(medium_primes, next_medium_idx_, &medium_, false, high_n, low_n, k_low);

        // EratBig-style activation (see header comment): find the smallest
        // multiplier m coprime to 210 (not just 30) with p*m >= max(p*p,
        // low_n), then pack qp/residue-class/mod-210 phase into one word
        // exactly like the dense tiers' DenseState, and file it directly
        // into the bucket ring by byte position (shift/mask, no division).
        while (next_sparse_idx_ < sparse_primes.size()) {
            uint64_t p = sparse_primes[next_sparse_idx_];
            if (p * p >= high_n) break;
            uint64_t start_val = std::max(p * p, low_n);
            uint64_t m = (start_val + p - 1) / p;
            uint64_t t = m / 210, sres = m % 210;
            uint64_t w = big::NEXT_W[sres];
            if (w == 48) { ++t; w = 0; }
            m = t * 210 + big::M210[w];
            uint64_t n = p * m;
            uint64_t pos = n / WHEEL_MOD - k_low / 8;
            uint64_t ri = static_cast<uint64_t>(WHEEL_POS[p % WHEEL_MOD]);
            erat::DenseState e{static_cast<uint32_t>(((p / WHEEL_MOD) << 9) | (ri * 48 + w)), 0};
            uint64_t ahead = pos >> log2_sb_;
            e.pos = static_cast<uint32_t>(pos & ((uint64_t{1} << log2_sb_) - 1));
            if (ahead >= num_buckets_) {
                throw std::runtime_error(
                    "bucket sieve: salto de un primo disperso mayor que el margen del anillo de "
                    "cubos (bug de dimensionamiento en el constructor de SegmentSieve)");
            }
            push_sparse_entry(static_cast<uint32_t>((cur_segment_ + ahead) & (num_buckets_ - 1)), e);
            ++next_sparse_idx_;
        }

        // Small tier, one L1-sized sub-block at a time: presieve fill,
        // then every small prime crossed off inside that sub-block while
        // it's still L1-resident, instead of each prime sweeping the whole
        // (L2-sized) segment. Pending hits stay relative to the segment's
        // first byte until the last sub-block rebases them.
        uint8_t* bytes = reinterpret_cast<uint8_t*>(words_.data());
        for (uint64_t sb = 0; sb < bytes_needed; sb += sub_block_bytes_) {
            uint64_t se = std::min(sb + sub_block_bytes_, bytes_needed);
            uint64_t sb_bit = sb * 8;
            presieve_.fill(words_.data() + sb / 8, k_low + sb_bit, std::min<uint64_t>(count - sb_bit, (se - sb) * 8));
            uint64_t rebase = (se == bytes_needed) ? bytes_needed : 0;
            erat::cross_off_class<0>(bytes, se, small_[0].data(), small_[0].data() + small_[0].size(), rebase);
            erat::cross_off_class<1>(bytes, se, small_[1].data(), small_[1].data() + small_[1].size(), rebase);
            erat::cross_off_class<2>(bytes, se, small_[2].data(), small_[2].data() + small_[2].size(), rebase);
            erat::cross_off_class<3>(bytes, se, small_[3].data(), small_[3].data() + small_[3].size(), rebase);
            erat::cross_off_class<4>(bytes, se, small_[4].data(), small_[4].data() + small_[4].size(), rebase);
            erat::cross_off_class<5>(bytes, se, small_[5].data(), small_[5].data() + small_[5].size(), rebase);
            erat::cross_off_class<6>(bytes, se, small_[6].data(), small_[6].data() + small_[6].size(), rebase);
            erat::cross_off_class<7>(bytes, se, small_[7].data(), small_[7].data() + small_[7].size(), rebase);
        }
        // Wheel index 0 is the number 1: not prime, and nothing marks it.
        if (k_low == 0) words_[0] |= 1;

        // Medium tier: few hits per sub-block, so one pass over the whole
        // segment each.
        erat::cross_off_medium(words_.data(), count, medium_.data(), medium_.data() + medium_.size(), count);

        // Sparse tier: see process_sparse_bucket below (pulled out of this
        // function on purpose -- see its own comment). sparse_primes is
        // either empty for the whole run or not -- never changes segment
        // to segment -- so skipping the call entirely when it's empty
        // avoids paying a real (non-inlined) call's overhead every single
        // segment for N where this tier never has anything to do (every N
        // tested up to 1e12 on this machine, see README#benchmarks).
        if (!sparse_primes.empty()) process_big();
        ++cur_segment_;

        // Extraction: bit=0 => prime candidate. Accumulated locally and
        // added to prime_count once at the end, instead of read-modify-
        // writing through the reference every word (count-only) or every
        // single prime (value extraction) -- prime_count is a reference
        // into the caller's frame, so the compiler can't always prove
        // nothing else aliases it and keep it in a register across this
        // loop; a local can't be aliased by anything, so it stays in a
        // register for the whole function.
        uint64_t local_prime_count = 0;
        for (size_t w = 0; w < words_needed; ++w) {
            uint64_t bits = ~words_[w];
            uint64_t base_idx = w * 64ULL;
            uint64_t remaining = count - base_idx;
            if (remaining < 64) {
                bits &= (remaining == 0) ? 0ULL : ((1ULL << remaining) - 1ULL);
            }
            if (bits == 0) continue;

            // count-only (NullSink) never looks at the value written --
            // skip straight to how many bits are set instead of decoding
            // each one (ctz + wheel-index math) just to discard it below.
            if constexpr (!Writer::WANTS_VALUES) {
                local_prime_count += static_cast<uint64_t>(__builtin_popcountll(bits));
                continue;
            }

            uint64_t k_word_start = k_low + base_idx;
            uint64_t q = k_word_start / WHEEL_SIZE;
            uint64_t r = k_word_start % WHEEL_SIZE;

            uint64_t prev_bit = 0;
            while (bits) {
                uint64_t bit_pos = static_cast<uint64_t>(__builtin_ctzll(bits));
                uint64_t step2 = bit_pos - prev_bit;
                prev_bit = bit_pos;
                if constexpr (WHEEL_SIZE_IS_POW2) {
                    uint64_t rq = r + step2;
                    q += rq >> WHEEL_SIZE_LOG2;
                    r = rq & (static_cast<uint64_t>(WHEEL_SIZE) - 1);
                } else {
                    r += step2;
                    while (r >= static_cast<uint64_t>(WHEEL_SIZE)) { r -= WHEEL_SIZE; ++q; }
                }

                uint64_t value = q * WHEEL_MOD + WHEEL_R[r];
                out.write_uint64(value);
                ++local_prime_count;
                bits &= bits - 1; // clear the lowest set bit
            }
        }
        prime_count += local_prime_count;
    }

private:
    // Activates (appends state for) every prime in `primes` from `next`
    // on whose square falls below this segment's end; `primes` is sorted,
    // so this touches each prime exactly once per chunk. by_class: the
    // small tier -- byte positions, and one output list per residue class
    // (state[pr]); otherwise bit positions, all into state[0].
    static void activate_dense(const std::vector<uint64_t>& primes, size_t& next,
                               std::vector<erat::DenseState>* state, bool by_class,
                               uint64_t high_n, uint64_t low_n, uint64_t k_low) {
        while (next < primes.size()) {
            uint64_t p = primes[next];
            if (p * p >= high_n) break;

            // Smallest m coprime with WHEEL_MOD with p*m >= max(p*p, low_n).
            uint64_t start_val = std::max(p * p, low_n);
            uint64_t m = (start_val + p - 1) / p;
            uint64_t r = m % WHEEL_MOD;
            uint64_t step = STEP_TO_COPRIME[r];
            m += step;
            r += step;
            if (r >= WHEEL_MOD) r -= WHEEL_MOD;

            // Byte of p*m is (p*m)/30 (k = byte*8 + bit, see erat_small.hpp).
            uint64_t pos = by_class ? (p * m) / WHEEL_MOD - k_low / 8 : wheel_index(p * m) - k_low;
            uint64_t pr = static_cast<uint64_t>(WHEEL_POS[p % WHEEL_MOD]);
            uint64_t j = static_cast<uint64_t>(WHEEL_POS[r]);
            state[by_class ? pr : 0].push_back({static_cast<uint32_t>(((p / WHEEL_MOD) << 6) | (pr << 3) | j),
                             static_cast<uint32_t>(pos)});
            ++next;
        }
    }

    // Processes exactly the sparse-tier entries due this segment: mark,
    // advance, reschedule into whichever future bucket the next hit lands
    // in -- see the block-pool comment right below (attempt 6) for how a
    // due entry gets there and where it goes next.
    //
    // Pulled out of sieve_and_emit into its own function, and marked
    // noinline to make sure it stays that way even under -O3/-flto: with
    // dense/onfly/sparse all fully inlined into one function, perf showed
    // real cycles going to a spilled-to-stack reload of a loop-invariant
    // member (num_buckets_) inside what's now schedule_sparse -- the
    // *number* of values simultaneously live across all three tiers was
    // forcing spills, not a poor choice of which value to spill. Passing
    // values in as explicit parameters instead of member reads didn't
    // change anything measured (see git history) because that doesn't
    // reduce how many values are live at once, only where they come from.
    // Giving this tier its own function gives it its own register
    // allocation scope instead, so its live ranges stop competing with
    // the other two tiers' for the same register file. Confirmed with
    // perf stat, not just wall-clock (which turned out noisy for this
    // tier -- see README#benchmarks): at N=1e12 with a third of base
    // primes forced sparse (-s 500000), cycles dropped ~5-9% and IPC rose
    // from 1.07 to 1.14-1.19 across repeated runs, consistently. The
    // flip side of a real function call is real call overhead, paid once
    // per segment even when this tier has nothing due -- sieve_and_emit
    // below skips the call entirely when sparse_primes is empty for the
    // whole run (every N up to 1e12 tested on this machine), which is
    // what keeps the dense-only case's numbers unchanged from before this
    // split.
    // Attempt 6 (this session): replaces the idx-indexed intrusive list
    // (sparse_kj_/sparse_next_/sparse_primes, attempts 1-5 above) with
    // fixed-size blocks of erat::DenseState pulled from a pool, one queue
    // (linked list of blocks) per ring slot -- primesieve's own EratBig
    // design. The entry itself now carries everything needed to process
    // it (qp/pr/j packed into `qw`, `pos` relative to whichever segment
    // it's due in, same layout as the dense tiers' DenseState), so
    // rescheduling COPIES the entry into the target slot's tail block
    // instead of relinking an index -- both the read (draining a slot's
    // blocks front to back) and the write (appending to a tail) are
    // sequential, unlike the old scheme's pointer-chase over sparse_kj_/
    // sparse_next_/sparse_primes at effectively random idx values. This is
    // NOT attempt 3's AoS relayout (which kept the idx-array pointer-chase
    // and only repacked its fields, and lost to cache-footprint growth) --
    // here nothing is indexed by idx at all any more, and a live entry
    // costs exactly 8 bytes (sizeof(DenseState)) at any one time, less
    // than attempt 5's 12 bytes/active-prime (8 for sparse_kj_ + 4 for
    // sparse_next_, p amortized via the shared sparse_primes array).
    //
    // SPARSE_BLOCK_ENTRIES matters more than it looks: a first pass at
    // 1024 (8 KiB/block, primesieve's own EratBig default) won cleanly at
    // the *natural* N=1e13 cliff (cycles:u 23.16T->20.81T, -10.2%;
    // wall-clock 506.64s->449.13s; IPC 1.25->1.41; cache-misses
    // 22.00B->17.22B) but *lost* on this file's usual fast proxy (N=1e12,
    // -s 500000, 84% sparse forced): cycles:u 3.484T->3.775T, +8.3%;
    // wall-clock 75.43s->90.83s, +20.4% -- the opposite of every other
    // attempt in this tier's history, where the proxy and the natural N
    // agreed on direction even when they disagreed on magnitude. Root
    // cause: the proxy's tiny forced segment width needs many more ring
    // slots (num_buckets_ scales with 1/seg_k_width_), so its ~66k active
    // sparse primes per chunk spread thin across ~64 slots -- ~86 live
    // entries/slot, each getting its own mostly-empty 1024-entry block
    // (cache-references 84.79B, next to all of it pool padding no prime
    // ever occupies). The natural N=1e13 cliff has far fewer ring slots
    // (a much wider auto segment) and a comparable population, so its
    // blocks stay reasonably full and never hit this. Shrinking to 128
    // (1 KiB/block) fixed the proxy without giving back the natural-N win
    // -- both now agree: proxy cycles:u 3.484T->3.190T (-8.4%), cache-refs
    // 84.79B->18.49B (-78%), cache-misses 2.10B->0.258B (-87.7%); natural
    // N=1e13 cycles:u 23.16T->20.78T (-10.3%), wall-clock 506.64s->447.74s
    // (-11.6%), cache-refs 400.5B->295.8B (-26.1%). Kept at 128 -- if this
    // tier's population/ring-slot ratio changes a lot on a future
    // machine or N, re-check both regimes again rather than assuming
    // either one predicts the other for a block-size change specifically.
    //
    // Attempt 7 (tried, reverted): schedule_sparse's ring-slot placement
    // divides by seg_k_width_, a runtime value -- rounding main.cpp's
    // *auto*-computed width down to the nearest power of 2 (leaving an
    // explicit -s exactly as given) turns that into a shift. Measured at
    // both natural N this tier's history already tracks: N=1e12 cycles:u
    // 1.6571T->1.6646T (+0.45%, noise-level), cache-refs 14.30B->13.45B
    // (-5.9%); N=1e13 cycles:u 20.776T->20.818T (+0.2%, also noise-level)
    // but cache-refs 295.8B->333.9B (+12.9%) and cache-misses 16.20B->
    // 18.04B (+11.4%) -- worse, and in the OPPOSITE direction from N=1e12.
    // Net: no consistent win on the metric this tier's history actually
    // trusts (cycles:u flat both times, within noise), and the cache
    // impact of rounding the *width itself* down doesn't even agree in
    // sign between the two N tried, let alone offset what the shift
    // saves. Whatever the auto-tuned width was doing (see arg_parser.hpp's
    // own comment on the sqrt/L2 tradeoff) is sensitive enough that even
    // rounding it *down* -- the safe direction, unlike Finding 2's already-
    // reverted rounding-up-to-fill-L2 attempt -- isn't free. Reverted;
    // the division itself was never shown to cost anything on its own
    // here, only entangled with a width change that didn't pay for itself.
    // Drains this segment's ring slot: for each due entry, mark its one
    // hit (byte marking, mod-210 table lookup for mask/step -- see
    // wheel210_big.hpp), advance to the next hit, and re-file it (usually
    // into a future slot, occasionally the current one again if the step
    // is small) by byte position with a shift/mask instead of a division.
    // `pos` in a live entry is always relative to whichever segment it's
    // due in, so no k_low/k_high parameters are needed here at all, unlike
    // the old scheme.
    __attribute__((noinline))
    void process_big() {
        const uint32_t slot = static_cast<uint32_t>(cur_segment_ & (num_buckets_ - 1));
        uint8_t* const s = reinterpret_cast<uint8_t*>(words_.data());
        const uint32_t log2sb = log2_sb_;
        const uint64_t modsb = (uint64_t{1} << log2sb) - 1;
        const uint64_t bmask = num_buckets_ - 1;
        const uint64_t cur = cur_segment_;
        while (head_[slot]) {
            Blk* blk = head_[slot];
            erat::DenseState* last_end = tail_[slot];
            head_[slot] = nullptr;
            tail_[slot] = nullptr;
            while (blk) {
                Blk* next_blk = blk->next;
                erat::DenseState* it = blk->entries();
                erat::DenseState* end = next_blk ? blk->block_end() : last_end;
                for (; it != end; ++it) {
                    uint32_t qw = it->qw;
                    uint64_t pos = it->pos;
                    uint64_t a = qw >> 9;
                    uint32_t idx = qw & 511;
                    const big::Entry& te = big::TABLE[idx];
                    s[pos] |= te.mask;
                    pos += a * te.dm + te.corr;
                    uint64_t ahead = pos >> log2sb;
                    erat::DenseState e{static_cast<uint32_t>((a << 9) | te.next), static_cast<uint32_t>(pos & modsb)};
                    push_sparse_entry(static_cast<uint32_t>((cur + ahead) & bmask), e);
                }
                free_.push_back(blk);
                blk = next_blk;
            }
        }
    }

    // Blocks are BLK_BYTES-aligned: a tail pointer that lands exactly on a
    // BLK_BYTES boundary means "block full" (primesieve's Bucket trick) --
    // no count field to load on every push. Appends `entry` to ring slot
    // `slot`'s tail block, starting a new one if the current tail is full
    // or the slot is empty.
    void push_sparse_entry(uint32_t slot, erat::DenseState entry) {
        erat::DenseState* w = tail_[slot];
        if (w == nullptr || (reinterpret_cast<uintptr_t>(w) & (BLK_BYTES - 1)) == 0) {
            Blk* nb = alloc_blk();
            nb->next = nullptr;
            if (w == nullptr) head_[slot] = nb;
            else reinterpret_cast<Blk*>(reinterpret_cast<char*>(w) - BLK_BYTES)->next = nb;
            w = nb->entries();
        }
        *w = entry;
        tail_[slot] = w + 1;
    }

    // BLK_BYTES-aligned blocks pulled from a pool of aligned_alloc'd
    // CHUNK_BLOCKS-sized arenas (indices/pointers into chunks_ stay valid
    // across pool growth since chunks_ holds owning pointers, never moved
    // or resized in place). free_ is a stack of blocks not currently in
    // any ring slot; begin_chunk() repopulates it from every arena ever
    // allocated, never shrinking the pool.
    static constexpr size_t BLK_BYTES = 1024;
    static constexpr size_t CHUNK_BLOCKS = 256;
    struct Blk {
        Blk* next;
        uint64_t pad;
        erat::DenseState* entries() { return reinterpret_cast<erat::DenseState*>(this + 1); }
        erat::DenseState* block_end() { return reinterpret_cast<erat::DenseState*>(reinterpret_cast<char*>(this) + BLK_BYTES); }
    };
    struct AlignedFree { void operator()(char* p) const { std::free(p); } };
    Blk* alloc_blk() {
        if (free_.empty()) {
            char* c = static_cast<char*>(std::aligned_alloc(BLK_BYTES, BLK_BYTES * CHUNK_BLOCKS));
            chunks_.emplace_back(c);
            for (size_t i = 0; i < CHUNK_BLOCKS; ++i) free_.push_back(reinterpret_cast<Blk*>(c + i * BLK_BYTES));
        }
        Blk* b = free_.back();
        free_.pop_back();
        return b;
    }
    std::vector<Blk*> head_;
    std::vector<erat::DenseState*> tail_;
    std::vector<Blk*> free_;
    std::vector<std::unique_ptr<char, AlignedFree>> chunks_;

    std::vector<uint64_t> words_;
    uint64_t seg_k_width_;
    uint64_t sub_block_bytes_;
    const Presieve& presieve_;

    // Dense tiers' per-prime state (erat_small.hpp), in lockstep with
    // small_primes/medium_primes as they activate -- no bucket, walked
    // every segment (small: every sub-block).
    std::vector<erat::DenseState> small_[8]; // one list per residue class p % 30
    std::vector<erat::DenseState> medium_;

    uint32_t log2_sb_ = 0; // log2(segment width in bytes) -- see constructor
    uint64_t num_buckets_ = 1;
    uint64_t cur_segment_ = 0;

    size_t next_small_idx_ = 0;
    size_t next_medium_idx_ = 0;
    size_t next_sparse_idx_ = 0;
};
