#pragma once
// "Sinks" for SegmentSieve: three ways to consume the primes it finds.
//
//  - NullSink: does nothing at all. Used for --count-only, where we only
//    care about the total count (already tracked by sieve_and_emit itself)
//    and want to skip both the to_chars conversion and any I/O.
//
//  - ByteCounter: writes nothing, just counts how many bytes the result
//    would take as text (digits + newline). Used in a first pass, with no
//    I/O, to know exactly where each thread must start writing.
//
//  - DirectWriter: writes with pwrite() at an absolute position in the
//    final, already-sized file. Since each thread owns a disjoint byte
//    range, all of them can write in parallel on the same descriptor with
//    no locking and no later merge step.

#include <cstdint>
#include <cstring>
#include <charconv>
#include <vector>
#include <stdexcept>
#include <unistd.h>

struct NullSink {
    // Tells SegmentSieve::sieve_and_emit's extraction loop it never needs
    // an actual prime value out of a set bit -- --count-only (the only
    // user of NullSink) only wants how many bits are set, so that loop can
    // skip straight to a popcount per word instead of decoding each one
    // (ctz + wheel-index-to-value math) just to hand it to this no-op.
    static constexpr bool WANTS_VALUES = false;
    void write_uint64(uint64_t) {}
};

struct ByteCounter {
    static constexpr bool WANTS_VALUES = true;
    uint64_t total_bytes = 0;

    void write_uint64(uint64_t v) {
        char scratch[24];
        auto res = std::to_chars(scratch, scratch + sizeof(scratch), v);
        total_bytes += static_cast<uint64_t>(res.ptr - scratch) + 1; // +1 for '\n'
    }
};

class DirectWriter {
public:
    static constexpr bool WANTS_VALUES = true;

    DirectWriter(int fd, uint64_t start_offset, size_t buffer_size = (1u << 22)) // 4 MiB
        : fd_(fd), offset_(start_offset), buf_(buffer_size) {}

    DirectWriter(const DirectWriter&) = delete;
    DirectWriter& operator=(const DirectWriter&) = delete;

    ~DirectWriter() { try { flush(); } catch (...) {} }

    void write_uint64(uint64_t v) {
        if (pos_ + 21 > buf_.size()) flush();
        auto res = std::to_chars(buf_.data() + pos_, buf_.data() + buf_.size(), v);
        pos_ = static_cast<size_t>(res.ptr - buf_.data());
        buf_[pos_++] = '\n';
    }

    void write_raw(const char* data, size_t len) {
        if (pos_ + len > buf_.size()) flush();
        std::memcpy(buf_.data() + pos_, data, len);
        pos_ += len;
    }

    void flush() {
        size_t total_written = 0;
        while (total_written < pos_) {
            ssize_t w = ::pwrite(fd_, buf_.data() + total_written, pos_ - total_written,
                                  static_cast<off_t>(offset_ + total_written));
            if (w < 0) throw std::runtime_error("pwrite failed writing the output file");
            total_written += static_cast<size_t>(w);
        }
        offset_ += pos_;
        pos_ = 0;
    }

private:
    int fd_;
    uint64_t offset_;
    std::vector<char> buf_;
    size_t pos_ = 0;
};
