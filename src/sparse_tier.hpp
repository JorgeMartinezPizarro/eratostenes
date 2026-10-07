#pragma once
// The sparse tier (primesieve's EratBig design): the base primes with about
// one hit per segment or fewer. Most segments have nothing to do for most
// of them, so each one waits in the ring slot of the segment its next hit
// falls in, and a segment only touches the primes due in it. One per
// SegmentSieve (segment_sieve.hpp), which owns the segment bytes and calls
// activate / process_big / next_segment once per segment. docs/ALGORITHM.md
// §6 has the design, docs/RESEARCH.md the history.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

// madvise(MADV_HUGEPAGE) for the ring's arenas (huge_arenas, decided in
// tuning.hpp). Best effort: THP in "madvise" or "always" mode.
#include <sys/mman.h>
#ifdef __BMI2__
#include <immintrin.h>
#endif

#include "base_sieve.hpp"
#include "erat_small.hpp"
#include "wheel.hpp"
#include "wheel210_big.hpp"

// process_big prefetches the segment byte of the entries this many
// positions ahead in the bucket (see its comment); 0 turns it off.
#ifndef ERA_BIG_PF
#define ERA_BIG_PF 16
#endif
// Activation: ERA_ACT_IDX takes p / 30 and p % 30 from the prime's wheel
// index instead of dividing. Fewer instructions and faster on modern cores,
// but slower on Ivy Bridge (its 64-bit division stalls longer once the
// independent work around it is gone), so it is on only where BMI2 exists.
// See docs/RESEARCH.md#sparse-activation-from-the-bitmap-index-18-fewer-instructions-per-prime-kept-2026-10-05.
#ifndef ERA_ACT_IDX
#ifdef __BMI2__
#define ERA_ACT_IDX 1
#else
#define ERA_ACT_IDX 0
#endif
#endif
// Bucket block size (bytes); -DERA_BLK_BYTES for A/B.
#ifndef ERA_BLK_BYTES
#define ERA_BLK_BYTES 4096
#endif

// v mod 2^nbits, with mask = 2^nbits - 1: one BMI2 instruction (bzhi) that
// takes the bit count from a register, so process_big's loop needn't keep
// the mask live; plain `and` where there is no BMI2 (Ivy Bridge, the
// portable build).
static inline uint64_t low_bits(uint64_t v, uint32_t nbits, uint64_t mask) {
#ifdef __BMI2__
    (void)mask;
    return _bzhi_u64(v, nbits);
#else
    (void)nbits;
    return v & mask;
#endif
}

class SparseTier {
public:
    // seg_bytes: the segment width in bytes, a power of 2 whenever the run
    // has sparse primes (has_sparse; the ring's slot math is a shift --
    // tuning.hpp rounds it, this checks). base_prime_max: the largest base
    // prime (isqrt(limit)); sizes the ring so no prime's step can wrap
    // around it.
    SparseTier(uint64_t seg_bytes, uint64_t base_prime_max, bool has_sparse, bool huge_arenas)
        : arena_bytes_(huge_arenas ? (size_t{2} << 20) : BLK_BYTES * 256),
          huge_arenas_(huge_arenas) {
        if (has_sparse && (seg_bytes & (seg_bytes - 1))) {
            throw std::runtime_error("sparse tier: needs a power-of-2 segment (bytes)");
        }
        while ((uint64_t{1} << log2_sb_) < seg_bytes) ++log2_sb_;
        sb_mask_ = (uint64_t{1} << log2_sb_) - 1;
        // The packed entry (see file_sparse) holds pos in 24 bits and qp in 28.
        if (log2_sb_ > 24 || base_prime_max / WHEEL_MOD >= (uint64_t{1} << 28)) {
            throw std::runtime_error("sparse tier: segment or base prime too large for the packed entry");
        }
        // Largest BYTE step between one prime's consecutive hits: qp *
        // max(dm) + max(corr), with max(dm) = 14 on the mod-2310 multiplier
        // wheel (the largest gap between consecutive 2310-coprime residues)
        // -- a few extra WHEEL_SIZE's of slack (+16) cost nothing (buckets
        // are cheap) and keep this comfortably safe.
        uint64_t maxstep = base_prime_max / WHEEL_MOD * 14 + 16;
        uint64_t ahead = (maxstep >> log2_sb_) + 2;
        num_buckets_ = 1;
        while (num_buckets_ < ahead * 2) num_buckets_ <<= 1; // power of 2, 2x margin
        // Twice num_buckets_ slots, so a hit's slot is plainly cur_segment_ +
        // ahead (ahead < num_buckets_, cur_segment_ < num_buckets_) and the
        // hot loop has no wrap mask; wrap_ring() shifts the upper half down
        // once the cursor reaches num_buckets_. See process_big.
        head_.assign(2 * num_buckets_, nullptr);
        tail_.assign(2 * num_buckets_, nullptr);
    }

    // A new run of consecutive segments (SegmentSieve::begin_chunk): empty
    // ring, cursor at slot 0, activation from the run's own first prime.
    void begin_chunk() {
        cur_segment_ = 0;
        next_k_ = 0;
        std::fill(head_.begin(), head_.end(), nullptr);
        std::fill(tail_.begin(), tail_.end(), nullptr);
        // Blocks aren't freed, just handed back to the pool: every block
        // ever allocated here is reusable, so repopulate the free list from
        // scratch rather than reallocate.
        free_.clear();
        for (auto& c : chunks_)
            for (size_t i = 0; i < arena_bytes_ / BLK_BYTES; ++i)
                free_.push_back(reinterpret_cast<Blk*>(c.get() + i * BLK_BYTES));
    }

    // Files every prime of `primes` whose square falls below high_n (this
    // segment's exclusive numeric end) and that isn't filed yet; returns how
    // many. The primes are a run of the base-prime bitmap (base_sieve.hpp's
    // SparsePrimes), walked up to k_stop, the first index whose prime's
    // square reaches high_n (one isqrt per segment instead of a p * p compare
    // per prime).
    size_t activate(const SparsePrimes& primes, uint64_t high_n, uint64_t low_n, uint64_t k_low) {
        size_t activated = 0;
        if (next_k_ < primes.k_begin) next_k_ = primes.k_begin;
        const uint64_t k_cut = wheel_count_upto(isqrt(high_n - 1)); // primes with p*p < high_n have k < k_cut
        const uint64_t k_stop = std::min(k_cut, primes.k_end);
        while (next_k_ < k_stop) {
            const uint64_t wi = next_k_ >> 6;
            const uint64_t word_end = std::min((wi + 1) << 6, k_stop);
            uint64_t bits = primes.words[wi] & (~uint64_t{0} << (next_k_ & 63));
            if (word_end & 63) bits &= (uint64_t{1} << (word_end & 63)) - 1; // partial last word
            while (bits) {
                file_sparse((wi << 6) + static_cast<uint64_t>(__builtin_ctzll(bits)), low_n, k_low);
                ++activated;
                bits &= bits - 1;
            }
            next_k_ = word_end;
        }
        return activated;
    }

    // Drains this segment's slot into the segment bytes `s`: for each entry,
    // mark its hit, step to the next one with the mod-2310 table
    // (big::TABLE2310; 11 is presieved too, ~9% fewer hits than mod 210) and
    // copy the entry into the tail block of the slot that hit falls in. The
    // entry is idx (class and phase) | pos << 12 | qp << 36, pos relative to
    // the segment it is due in. noinline: its own register allocation, away
    // from the dense tiers'. See
    // docs/RESEARCH.md#sparse-tier-design-current-fixed-size-pooled-blocks-attempt-6
    // and docs/RESEARCH.md#sparse-tier-mod-2310-multiplier-wheel-kept-2026-09-30.
    //
    // Ring slots: this segment's is cur_segment_ (kept below num_buckets_,
    // see wrap_ring), a hit `ahead` segments on files into cur_segment_ +
    // ahead, always below the 2 x num_buckets_ slots allocated (the ring's
    // sizing has ahead < num_buckets_ / 2 for a re-filed hit, and
    // file_sparse checks ahead < num_buckets_ for an activation), so the
    // loop needs no wrap mask.
    __attribute__((noinline))
    void process_big(uint8_t* const s) {
        const uint32_t slot = static_cast<uint32_t>(cur_segment_);
        // This segment's tail pointer, as a pointer: a hit `ahead` segments
        // on files into tails_cur[ahead], so neither the array base (which
        // the s[pos] store would force GCC to reload) nor the cursor is live
        // in the loop. The slow path recovers the slot index from
        // tail_.data().
        erat::DenseState** const tails_cur = tail_.data() + cur_segment_;
        // uint32_t on purpose: as uint64_t GCC kept two copies and spilled
        // tails_cur (docs/RESEARCH.md).
        const uint32_t log2sb = log2_sb_;
        const uint64_t modsb = (uint64_t{1} << log2sb) - 1;
        while (head_[slot]) {
            Blk* blk = head_[slot];
            erat::DenseState* last_end = tail_[slot];
            head_[slot] = nullptr;
            tail_[slot] = nullptr;
            while (blk) {
                Blk* next_blk = blk->next;
                // The blocks of a chain are scattered in memory (LIFO free
                // list), so the next one is prefetched into L2 a line at a
                // time over this block's first half. The last block of a
                // chain prefetches itself (already cached), so the loops
                // below test nothing for it. See
                // docs/RESEARCH.md#sparse-tier-next-block-prefetch-spread-over-the-current-block-kept-2026-10-04.
                const char* nb = reinterpret_cast<const char*>(next_blk ? next_blk : blk);
                erat::DenseState* it = blk->entries();
                erat::DenseState* end = next_blk ? blk->block_end() : last_end;
                // Two entries per step: both entries' loads, table rows and
                // segment RMWs are issued before either push, so their misses
                // overlap (EratBig's loop shape). The pushes stay in order: a
                // shared slot's second push reads the tail the first one just
                // wrote. Each entry and table row is one 8-byte load, the new
                // entry one 8-byte store, the new-block path out of line.
                constexpr int U = 2;
                // The segment byte of the entries ERA_BIG_PF ahead of p, cnt
                // of them (inside the block; the caller keeps it so): that RMW
                // is the load that misses once two threads share an L2. See
                // docs/RESEARCH.md#sparse-tier-two-entries-per-iteration-in-process_big-and-a-segment-byte-prefetch-16-entries-ahead-both-kept-2026-10-02.
                auto seg_pf = [&](const erat::DenseState* p, size_t cnt) __attribute__((always_inline)) {
                    if constexpr (ERA_BIG_PF > 0) {
                        for (size_t k = 0; k < cnt; ++k) {
                            uint64_t pe;
                            std::memcpy(&pe, p + ERA_BIG_PF + k, sizeof(uint64_t));
                            __builtin_prefetch(s + ((pe >> 12) & 0xffffff), 1, 3);
                        }
                    } else {
                        (void)p; (void)cnt;
                    }
                };
                // The U entries [p, p + U): loads, table rows and segment
                // RMWs first, then the pushes in order.
                auto body = [&](const erat::DenseState* p) __attribute__((always_inline)) {
                    uint64_t ent[U], pos[U], te[U], e[U], sl[U];
                    for (int k = 0; k < U; ++k) std::memcpy(&ent[k], p + k, sizeof(uint64_t));
                    for (int k = 0; k < U; ++k) {
                        pos[k] = (ent[k] >> 12) & 0xffffff;
                        te[k] = big::TABLE2310[ent[k] & 4095];
                    }
                    for (int k = 0; k < U; ++k) s[pos[k]] |= static_cast<uint8_t>(te[k]);
                    uint64_t nidx[U];
                    for (int k = 0; k < U; ++k) {
                        pos[k] += (ent[k] >> 36) * ((te[k] >> 8) & 0xff) + ((te[k] >> 16) & 15);
                        nidx[k] = te[k] >> 20;
                    }
                    for (int k = 0; k < U; ++k) {
                        sl[k] = pos[k] >> log2sb; // segments ahead: the slot is tails_cur[sl]
                        e[k] = (ent[k] & ~((uint64_t{1} << 36) - 1)) | nidx[k] | (low_bits(pos[k], log2sb, modsb) << 12);
                    }
                    for (int k = 0; k < U; ++k) {
                        erat::DenseState** const tp = tails_cur + sl[k];
                        erat::DenseState* w = *tp;
                        if ((reinterpret_cast<uintptr_t>(w) & (BLK_BYTES - 1)) == 0) [[unlikely]]
                            w = new_block(static_cast<uint32_t>(tp - tail_.data()));
                        std::memcpy(w, &e[k], sizeof(uint64_t));
                        *tp = w + 1;
                    }
                };
                // Groups of G entries, counted once per block: the first
                // `spread` groups also prefetch line g of the next block (64
                // lines over the block's first 256 entries), the rest only the
                // segment bytes ERA_BIG_PF ahead; the entries whose
                // ERA_BIG_PF-ahead neighbours are past `end` go through the
                // plain pairs. No test inside either loop but its own end. See
                // docs/RESEARCH.md#sparse-tier-process_big-in-groups-of-4-entries-no-per-iteration-edge-tests-kept-2026-10-06.
                constexpr size_t G = 4;
                constexpr size_t PF = ERA_BIG_PF > 0 ? ERA_BIG_PF : 0;
                const size_t n = static_cast<size_t>(end - it);
                const size_t groups = n >= PF + G ? (n - PF) / G : 0;
                const size_t spread = std::min<size_t>(groups, BLK_BYTES / 64);
                const char* line = nb;
                for (erat::DenseState* const ge = it + spread * G; it != ge; it += G) {
                    __builtin_prefetch(line, 0, 2);
                    line += 64;
                    seg_pf(it, G);
                    for (size_t k = 0; k < G; k += U) body(it + k);
                }
                for (erat::DenseState* const ge = it + (groups - spread) * G; it != ge; it += G) {
                    seg_pf(it, G);
                    for (size_t k = 0; k < G; k += U) body(it + k);
                }
                for (; it + U <= end; it += U) body(it);
                // An odd last entry, one at a time.
                for (; it != end; ++it) {
                    uint64_t ent;
                    std::memcpy(&ent, it, sizeof(ent)); // idx | pos << 12 | qp << 36
                    uint64_t pos = (ent >> 12) & 0xffffff;
                    const uint64_t te = big::TABLE2310[ent & 4095]; // mask | dm << 8 | corr << 16 | next << 20
                    s[pos] |= static_cast<uint8_t>(te);
                    pos += (ent >> 36) * ((te >> 8) & 0xff) + ((te >> 16) & 15);
                    const uint64_t e = (ent & ~((uint64_t{1} << 36) - 1)) | (te >> 20) | (low_bits(pos, log2sb, modsb) << 12);
                    erat::DenseState** const tp = tails_cur + (pos >> log2sb);
                    erat::DenseState* w = *tp;
                    // Null (empty slot) or on a block boundary (block full).
                    if ((reinterpret_cast<uintptr_t>(w) & (BLK_BYTES - 1)) == 0) [[unlikely]]
                        w = new_block(static_cast<uint32_t>(tp - tail_.data()));
                    std::memcpy(w, &e, sizeof(e));
                    *tp = w + 1;
                }
                free_.push_back(blk);
                blk = next_blk;
            }
        }
    }

    // Moves the cursor to the next segment's slot, after every segment.
    void next_segment() {
        if (++cur_segment_ == num_buckets_) wrap_ring();
    }

private:
    // Files the prime of wheel index k into the ring: the smallest
    // multiplier m coprime to 2310 with p*m >= max(p*p, low_n), packed into
    // one entry, into the slot of the segment that hit falls in. qp and the
    // residue class come from the index itself (ERA_ACT_IDX; k >> 3 and
    // k & 7), not from dividing p.
    void file_sparse(uint64_t k, uint64_t low_n, uint64_t k_low) {
        uint64_t qp, ri, p;
        if constexpr (ERA_ACT_IDX) {
            qp = k >> WHEEL_SIZE_LOG2;             // p / WHEEL_MOD
            ri = k & (WHEEL_SIZE - 1);             // WHEEL_POS[p % WHEEL_MOD]
            p = qp * WHEEL_MOD + WHEEL_R[ri];
        } else {
            // From the prime: a constant division and a constant modulo per prime.
            p = wheel_number(k);
            qp = p / WHEEL_MOD;
            ri = static_cast<uint64_t>(WHEEL_POS[p % WHEEL_MOD]);
        }
        uint64_t start_val = std::max(p * p, low_n);
        uint64_t m = (start_val + p - 1) / p;
        // Packed as one word: idx (ri * 480 + w) in bits 0-11, pos in 12-35,
        // qp in 36-63 -- see process_big.
        uint64_t t = m / 2310, sres = m % 2310;
        uint64_t w = big::NEXT_W2310[sres];
        if (w == big::W2310) { ++t; w = 0; }
        m = t * 2310 + big::M2310[w];
        uint64_t pos = p * m / WHEEL_MOD - k_low / 8;
        uint64_t ahead = pos >> log2_sb_;
        if (ahead >= num_buckets_) {
            throw std::runtime_error(
                "bucket sieve: a sparse prime's step exceeds the bucket ring's margin "
                "(sizing bug in SparseTier's constructor)");
        }
        uint64_t ent = (ri * big::W2310 + w) | ((pos & sb_mask_) << 12) | (qp << 36);
        erat::DenseState e;
        std::memcpy(&e, &ent, sizeof(e));
        push_sparse_entry(static_cast<uint32_t>(cur_segment_ + ahead), e);
    }

    // The ring's cursor reached num_buckets_: every slot below it has been
    // drained (process_big empties this segment's slot before moving on),
    // every pending entry sits in [num_buckets_, 2 x num_buckets_). Move
    // that half down and start the cursor over -- a few hundred pointers
    // once every num_buckets_ segments, in place of a mask on every hit.
    void wrap_ring() {
        const auto nb = static_cast<std::ptrdiff_t>(num_buckets_);
        std::copy(head_.begin() + nb, head_.end(), head_.begin());
        std::fill(head_.begin() + nb, head_.end(), nullptr);
        std::copy(tail_.begin() + nb, tail_.end(), tail_.begin());
        std::fill(tail_.begin() + nb, tail_.end(), nullptr);
        cur_segment_ = 0;
    }

    // Blocks are BLK_BYTES-aligned: a tail pointer that lands exactly on a
    // BLK_BYTES boundary means "block full" (primesieve's Bucket trick) --
    // no count field to load on every push. Appends `entry` to ring slot
    // `slot`'s tail block, starting a new one if the current tail is full
    // or the slot is empty.
    void push_sparse_entry(uint32_t slot, erat::DenseState entry) {
        erat::DenseState* w = tail_[slot];
        if ((reinterpret_cast<uintptr_t>(w) & (BLK_BYTES - 1)) == 0) w = new_block(slot);
        *w = entry;
        tail_[slot] = w + 1;
    }

    // Slow path of a push (tail_[slot] null or at a block boundary): links a
    // fresh block into ring slot `slot` and returns its first entry. Out of
    // line so process_big's loop keeps its constants in registers.
    __attribute__((noinline))
    erat::DenseState* new_block(uint32_t slot) {
        erat::DenseState* w = tail_[slot];
        Blk* nb = alloc_blk();
        nb->next = nullptr;
        if (w == nullptr) head_[slot] = nb;
        else reinterpret_cast<Blk*>(reinterpret_cast<char*>(w) - BLK_BYTES)->next = nb;
        return nb->entries();
    }

    // BLK_BYTES-aligned blocks pulled from a pool of aligned_alloc'd
    // arena_bytes_-sized arenas (indices/pointers into chunks_ stay valid
    // across pool growth since chunks_ holds owning pointers, never moved
    // or resized in place). free_ is a stack of blocks not currently in
    // any ring slot; begin_chunk() repopulates it from every arena ever
    // allocated, never shrinking the pool.
    static constexpr size_t BLK_BYTES = ERA_BLK_BYTES;
    struct Blk {
        Blk* next;
        uint64_t pad;
        erat::DenseState* entries() { return reinterpret_cast<erat::DenseState*>(this + 1); }
        erat::DenseState* block_end() { return reinterpret_cast<erat::DenseState*>(reinterpret_cast<char*>(this) + BLK_BYTES); }
    };
    struct AlignedFree { void operator()(char* p) const { std::free(p); } };
    Blk* alloc_blk() {
        if (free_.empty()) {
            char* c = static_cast<char*>(std::aligned_alloc(huge_arenas_ ? arena_bytes_ : BLK_BYTES, arena_bytes_));
            if (c && huge_arenas_) madvise(c, arena_bytes_, MADV_HUGEPAGE); // best effort
            chunks_.emplace_back(c);
            for (size_t i = 0; i < arena_bytes_ / BLK_BYTES; ++i) free_.push_back(reinterpret_cast<Blk*>(c + i * BLK_BYTES));
        }
        Blk* b = free_.back();
        free_.pop_back();
        return b;
    }
    std::vector<Blk*> head_;
    std::vector<erat::DenseState*> tail_;
    std::vector<Blk*> free_;
    std::vector<std::unique_ptr<char, AlignedFree>> chunks_;
    // Arena: 256 blocks (1 MiB), or one 2 MiB huge page (huge_arenas_).
    size_t arena_bytes_;
    bool huge_arenas_;

    uint32_t log2_sb_ = 0;     // log2(segment width in bytes) -- see constructor
    uint64_t sb_mask_ = 0;     // (1 << log2_sb_) - 1: a hit's byte offset inside its segment
    uint64_t num_buckets_ = 1;
    uint64_t cur_segment_ = 0; // this segment's ring slot, in [0, num_buckets_) -- see wrap_ring
    uint64_t next_k_ = 0;      // wheel index of the next prime to activate
};
