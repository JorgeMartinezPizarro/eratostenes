#pragma once
// Sink for .db (SQLite) output mode: buffers primes into fixed-size
// blocks, gap-encodes each block (gap_encoding.hpp), zstd-compresses it,
// and hands the finished block off via a callback (SqlitePrimeStore::push,
// see sqlite_prime_store.hpp) for a dedicated writer thread to insert.
//
// Same write_uint64(uint64_t) contract as NullSink/ByteCounter/DirectWriter
// in sinks.hpp, so sieve_chunk<Writer> (main.cpp) works unchanged. One
// GapBlockSink is owned per work chunk (main.cpp splits the range into many
// more chunks than threads -- see run_parallel_chunks); blocks from
// different chunks are NOT stitched together across chunk boundaries --
// each is self-describing (start_index, count), so only the last block of
// each chunk may come out shorter than block_size. That costs at most
// (chunk count) short blocks total out of what's otherwise hundreds of
// thousands -- negligible, and it avoids any cross-chunk coordination.
//
// start_index here is always CHUNK-RELATIVE (starts at 0), unlike before --
// there is no separate counting pre-pass any more to hand this sink its
// true global offset up front (see main.cpp's is_db_output block). Each
// block instead carries chunk_id, and SqlitePrimeStore::finish() corrects
// every block's start_index up to its real global value with a handful of
// cheap UPDATEs (one per chunk, not per block) once every chunk's actual
// prime count is known -- a count that now falls out of this same sieve
// pass for free (sieve_chunk's local_count), instead of a second full
// re-sieve whose only job was computing that count ahead of time. .db
// blocks are looked up by an index on start_index (nth_prime.cpp), not by
// insertion order, so this deferred fixup is invisible to any reader.

#include <cstdint>
#include <functional>
#include <stdexcept>
#include <vector>
#include <zstd.h>

#include "gap_encoding.hpp"

struct PendingBlock {
    uint64_t chunk_id;
    uint64_t start_index; // chunk-relative until SqlitePrimeStore::finish() fixes it up
    uint64_t count;
    uint64_t start_prime;
    std::vector<uint8_t> compressed;
};

class GapBlockSink {
public:
    static constexpr bool WANTS_VALUES = true;
    using PushFn = std::function<void(PendingBlock)>;

    GapBlockSink(uint64_t chunk_id, uint64_t block_size, int zstd_level, PushFn push)
        : chunk_id_(chunk_id), block_size_(block_size), zstd_level_(zstd_level),
          push_(std::move(push)) {
        raw_.reserve(block_size_ * 2); // gap bytes average well under 1/prime; generous headroom
        cbuf_.resize(ZSTD_compressBound(block_size_ * 5 + 16)); // worst case: every gap escapes (5 bytes)
    }

    GapBlockSink(const GapBlockSink&) = delete;
    GapBlockSink& operator=(const GapBlockSink&) = delete;

    ~GapBlockSink() { try { flush(); } catch (...) {} }

    void write_uint64(uint64_t p) {
        if (count_in_block_ == 0) {
            block_start_prime_ = p;
        } else {
            encode_gap(p - last_, raw_);
        }
        last_ = p;
        if (++count_in_block_ == block_size_) flush_block();
    }

    // Flushes a short final block (the last block of a thread's chunk).
    // Safe to call multiple times; a no-op once the block is empty.
    void flush() {
        if (count_in_block_ > 0) flush_block();
    }

private:
    void flush_block() {
        size_t csize = ZSTD_compress(cbuf_.data(), cbuf_.size(), raw_.data(), raw_.size(), zstd_level_);
        if (ZSTD_isError(csize)) throw std::runtime_error("zstd compression failed");

        PendingBlock blk;
        blk.chunk_id = chunk_id_;
        blk.start_index = start_index_;
        blk.count = count_in_block_;
        blk.start_prime = block_start_prime_;
        blk.compressed.assign(cbuf_.data(), cbuf_.data() + csize);
        push_(std::move(blk));

        start_index_ += count_in_block_;
        count_in_block_ = 0;
        raw_.clear();
    }

    uint64_t chunk_id_;
    uint64_t start_index_ = 0;
    uint64_t block_size_;
    int zstd_level_;
    PushFn push_;

    std::vector<uint8_t> raw_;
    std::vector<uint8_t> cbuf_;
    uint64_t count_in_block_ = 0;
    uint64_t block_start_prime_ = 0;
    uint64_t last_ = 0;
};
