#pragma once
// Sink for .db output mode: buffers primes into fixed-size blocks,
// gap-encodes each block (gap_encoding.hpp), zstd-compresses it, writes the
// bytes into the shared .blk file itself (block_file.hpp, parallel across
// threads) and hands the block's index row -- position, count, first prime,
// offset and length -- off via a callback (SqlitePrimeStore::push, see
// sqlite_prime_store.hpp) for a dedicated writer thread to insert.
//
// One GapBlockSink per chunk: blocks are not stitched across chunk
// boundaries (each is self-describing), so only a chunk's last block may be
// shorter than block_size. start_index is chunk-relative; each block carries
// its chunk_id, and SqlitePrimeStore::finish() adds the chunk's global
// offset once every chunk's prime count is known.

#include <cstdint>
#include <functional>
#include <stdexcept>
#include <vector>
#include <zstd.h>

#include "block_file.hpp"
#include "gap_encoding.hpp"

struct PendingBlock {
    uint64_t chunk_id;
    uint64_t start_index; // chunk-relative until SqlitePrimeStore::finish() fixes it up
    uint64_t count;
    uint64_t start_prime;
    uint64_t offset; // of the compressed block in the .blk file
    uint64_t len;
};

class GapBlockSink {
public:
    using PushFn = std::function<void(PendingBlock)>;

    GapBlockSink(uint64_t chunk_id, uint64_t block_size, int zstd_level, BlockFile& blocks, PushFn push)
        : chunk_id_(chunk_id), block_size_(block_size), zstd_level_(zstd_level), blocks_(blocks),
          push_(std::move(push)), raw_(block_size * MAX_GAP_BYTES), cbuf_(ZSTD_compressBound(raw_.size())) {}

    GapBlockSink(const GapBlockSink&) = delete;
    GapBlockSink& operator=(const GapBlockSink&) = delete;

    ~GapBlockSink() { try { flush(); } catch (...) {} }

    // One prime by value: the wheel's own primes, write_tiny_db's lists.
    void write_uint64(uint64_t p) {
        if (count_in_block_ == 0) block_start_prime_ = p;
        else raw_len_ = static_cast<size_t>(encode_gap(last_value(), p, raw_.data() + raw_len_) - raw_.data());
        last_ = p;
        last_on_wheel_ = on_wheel(p);
        if (last_on_wheel_) last_k_ = wheel_index(p);
        if (++count_in_block_ == block_size_) flush_block();
    }

    // Every prime of a sieved segment (SegmentSieve::sieve_and_emit): wheel
    // index k_low + i is prime when bit i of `words` is clear, for i < count.
    // Returns how many. The gap is k - the previous k; a value is only
    // rebuilt for a block's first prime and the rare escape. The encoder's
    // state stays in locals for the whole segment: written through a
    // uint8_t pointer, the bytes could alias any member, so member state
    // would be reloaded and stored on every prime.
    uint64_t write_segment(const uint64_t* words, uint64_t k_low, uint64_t count) {
        const uint64_t block_size = block_size_;
        uint8_t* const raw = raw_.data();
        uint8_t* p = raw + raw_len_;
        uint64_t in_block = count_in_block_;
        uint64_t last_k = last_k_;
        bool on = last_on_wheel_;
        uint64_t n = 0;
        const uint64_t words_needed = (count + 63) / 64;
        for (uint64_t w = 0; w < words_needed; ++w) {
            uint64_t bits = ~words[w];
            const uint64_t remaining = count - w * 64; // >= 1 for every w < words_needed
            if (remaining < 64) bits &= (uint64_t{1} << remaining) - 1;
            const uint64_t k_word = k_low + w * 64;
            n += static_cast<uint64_t>(__builtin_popcountll(bits));
            while (bits) {
                const uint64_t k = k_word + static_cast<uint64_t>(__builtin_ctzll(bits));
                bits &= bits - 1;
                if (in_block == 0) block_start_prime_ = wheel_number(k);
                else if (on && k - last_k <= 255) *p++ = static_cast<uint8_t>(k - last_k);
                else p = encode_gap(on ? wheel_number(last_k) : last_, wheel_number(k), p);
                last_k = k;
                on = true;
                if (++in_block == block_size) {
                    raw_len_ = static_cast<size_t>(p - raw);
                    count_in_block_ = in_block;
                    flush_block();
                    p = raw;
                    in_block = 0;
                }
            }
        }
        raw_len_ = static_cast<size_t>(p - raw);
        count_in_block_ = in_block;
        last_k_ = last_k;
        last_on_wheel_ = on;
        return n;
    }

    // Flushes a short final block (the last block of a thread's chunk).
    // Safe to call multiple times; a no-op once the block is empty.
    void flush() {
        if (count_in_block_ > 0) flush_block();
    }

private:
    // The previous prime's value: kept as a wheel index when it's on the
    // wheel (write_segment never computes the value), as the value otherwise.
    uint64_t last_value() const { return last_on_wheel_ ? wheel_number(last_k_) : last_; }

    void flush_block() {
        size_t csize = ZSTD_compress(cbuf_.data(), cbuf_.size(), raw_.data(), raw_len_, zstd_level_);
        if (ZSTD_isError(csize)) throw std::runtime_error("zstd compression failed");

        PendingBlock blk;
        blk.chunk_id = chunk_id_;
        blk.start_index = start_index_;
        blk.count = count_in_block_;
        blk.start_prime = block_start_prime_;
        blk.len = csize;
        blk.offset = blocks_.append(cbuf_.data(), csize);
        push_(std::move(blk));

        start_index_ += count_in_block_;
        count_in_block_ = 0;
        raw_len_ = 0;
    }

    uint64_t chunk_id_;
    uint64_t start_index_ = 0;
    uint64_t block_size_;
    int zstd_level_;
    BlockFile& blocks_;
    PushFn push_;

    std::vector<uint8_t> raw_;   // the block's gaps: at most MAX_GAP_BYTES per prime
    size_t raw_len_ = 0;
    std::vector<uint8_t> cbuf_;
    uint64_t count_in_block_ = 0;
    uint64_t block_start_prime_ = 0;
    uint64_t last_ = 0;          // previous prime's value, valid when !last_on_wheel_
    uint64_t last_k_ = 0;        // previous prime's wheel index, valid when last_on_wheel_
    bool last_on_wheel_ = false;
};
