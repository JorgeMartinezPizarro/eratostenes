#pragma once
// The .blk file of .db output mode: every compressed block, one after the
// other, written by the sieve threads themselves. A block's place is handed
// out by one atomic counter (fetch_add of its length) and written with
// pwrite() at that offset, so any number of threads append at once with no
// lock, no gaps and no merge step; blocks land in completion order,
// interleaved across threads, and the SQLite index (sqlite_prime_store.hpp)
// records each one's offset and length. Before this file existed the blocks
// went as BLOBs through the single SQLite writer thread, which capped .db
// output at ~260 MB/s on an NVMe RAID0 (i5-13500, 1e12: 77 s against a
// 43 s CPU floor) -- see docs/RESEARCH.md#db-output-where-the-time-goes.

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <fcntl.h>
#include <unistd.h>

// "dir/primes.db" -> "dir/primes.blk" (a ".db" suffix is replaced, anything
// else gets ".blk" appended).
inline std::string blk_path_for(const std::string& db_path) {
    const std::string suffix = ".db";
    if (db_path.size() >= suffix.size() &&
        std::equal(suffix.rbegin(), suffix.rend(), db_path.rbegin(),
                   [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); }))
        return db_path.substr(0, db_path.size() - suffix.size()) + ".blk";
    return db_path + ".blk";
}

class BlockFile {
public:
    explicit BlockFile(const std::string& path) : path_(path) {
        fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd_ < 0) throw std::runtime_error("could not create " + path + ": " + std::strerror(errno));
    }
    BlockFile(const BlockFile&) = delete;
    BlockFile& operator=(const BlockFile&) = delete;
    ~BlockFile() { if (fd_ >= 0) ::close(fd_); }

    // Thread-safe: writes `len` bytes at the next free offset and returns it.
    uint64_t append(const uint8_t* data, size_t len) {
        const uint64_t off = next_.fetch_add(len, std::memory_order_relaxed);
        size_t done = 0;
        while (done < len) {
            ssize_t w = ::pwrite(fd_, data + done, len - done, static_cast<off_t>(off + done));
            if (w < 0) {
                if (errno == EINTR) continue;
                throw std::runtime_error("pwrite failed writing " + path_ + ": " + std::strerror(errno));
            }
            done += static_cast<size_t>(w);
        }
        return off;
    }

    // Call once every writer has finished: flushes to disk and returns the
    // file's final size (what every append summed to).
    uint64_t finish() {
        if (::fsync(fd_) != 0) throw std::runtime_error("fsync failed on " + path_ + ": " + std::strerror(errno));
        return next_.load();
    }

    const std::string& path() const { return path_; }

private:
    std::string path_;
    int fd_ = -1;
    std::atomic<uint64_t> next_{0};
};
