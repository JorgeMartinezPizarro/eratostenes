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
    static constexpr bool WANTS_VALUES = true;
    using PushFn = std::function<void(PendingBlock)>;

    GapBlockSink(uint64_t chunk_id, uint64_t block_size, int zstd_level, BlockFile& blocks, PushFn push)
        : chunk_id_(chunk_id), block_size_(block_size), zstd_level_(zstd_level), blocks_(blocks),
          push_(std::move(push)) {
        raw_.reserve(block_size_ * 2); // ~1 byte per gap, rare 5-byte escapes; generous headroom
        cbuf_.resize(ZSTD_compressBound(block_size_ * 5 + 16)); // worst case: every gap escapes (5 bytes)
    }

    GapBlockSink(const GapBlockSink&) = delete;
    GapBlockSink& operator=(const GapBlockSink&) = delete;

    ~GapBlockSink() { try { flush(); } catch (...) {} }

    void write_uint64(uint64_t p) {
        if (count_in_block_ == 0) {
            block_start_prime_ = p;
        } else {
            encode_gap(last_value(), p, raw_);
        }
        last_ = p;
        last_on_wheel_ = on_wheel(p);
        if (last_on_wheel_) last_k_ = wheel_index(p);
        if (++count_in_block_ == block_size_) flush_block();
    }

    // Same encoding, fed the prime's wheel index instead of its value
    // (SegmentSieve::sieve_and_emit uses this when the sink has it): the
    // wheel gap is just k - last_k_, no value <-> index conversions. The
    // value is only rebuilt for a block's first prime and the rare escape.
    void write_k(uint64_t k) {
        if (count_in_block_ == 0) {
            block_start_prime_ = wheel_number(k);
        } else if (last_on_wheel_ && k - last_k_ <= 255) {
            raw_.push_back(static_cast<uint8_t>(k - last_k_));
        } else {
            encode_gap(last_value(), wheel_number(k), raw_);
        }
        last_k_ = k;
        last_on_wheel_ = true;
        if (++count_in_block_ == block_size_) flush_block();
    }

    // Flushes a short final block (the last block of a thread's chunk).
    // Safe to call multiple times; a no-op once the block is empty.
    void flush() {
        if (count_in_block_ > 0) flush_block();
    }

private:
    // The previous prime's value: kept as a wheel index when it's on the
    // wheel (write_k never computes the value), as the value otherwise.
    uint64_t last_value() const { return last_on_wheel_ ? wheel_number(last_k_) : last_; }

    void flush_block() {
        size_t csize = ZSTD_compress(cbuf_.data(), cbuf_.size(), raw_.data(), raw_.size(), zstd_level_);
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
        raw_.clear();
    }

    uint64_t chunk_id_;
    uint64_t start_index_ = 0;
    uint64_t block_size_;
    int zstd_level_;
    BlockFile& blocks_;
    PushFn push_;

    std::vector<uint8_t> raw_;
    std::vector<uint8_t> cbuf_;
    uint64_t count_in_block_ = 0;
    uint64_t block_start_prime_ = 0;
    uint64_t last_ = 0;          // previous prime's value, valid when !last_on_wheel_
    uint64_t last_k_ = 0;        // previous prime's wheel index, valid when last_on_wheel_
    bool last_on_wheel_ = false;
};
