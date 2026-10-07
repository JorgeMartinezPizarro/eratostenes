// Reads a .db produced by `eratostenes -o out.db` -- the SQLite index -- and
// its .blk sidecar (the gap-encoded, zstd-compressed blocks, one after the
// other; see gap_block_sink.hpp / block_file.hpp / sqlite_prime_store.hpp
// for the writer side, gap_encoding.hpp for the byte format shared by both),
// and answers queries on the stored primes without sieving them again: the
// Nth prime, the next prime after X, how many primes lie in [X, Y], or the
// primes themselves, by value or by position.
//
// Positions are 1-based and count from the first stored prime: pi(N) for a
// full run, but for a tail written with `--start S` position 1 is the first
// prime >= S (meta.range_start; pi(S - 1) is unknown without sieving [0, S)).
//
// Lookup: the block holding a position is the one with the largest
// start_index <= it (idx_blocks_start, an indexed query, not a table scan);
// by value, a binary search over positions with that same query (~20
// probes), comparing each block's start_prime -- not an SQL index on
// start_prime, which SQLite would order as a signed 64-bit integer, wrong
// above 2^63. A block is one pread() of its (offset, len) in the .blk, one
// zstd frame, and a gap walk: a few milliseconds per query whatever the
// file's size. --range / --slice stream the blocks in start_index order.

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sqlite3.h>
#include <zstd.h>

#include "arg_parser.hpp" // parse_size: numbers as eratostenes takes them (1e18, 100b, ...)
#include "colors.hpp"     // format_thousands
#include "gap_encoding.hpp"

static void print_nth_usage(const char* prog) {
    std::fprintf(stderr,
        "Usage: %s FILE.db N              the N-th stored prime (N >= 1)\n"
        "       %s FILE.db --count        how many primes the file stores\n"
        "       %s FILE.db --count X Y    how many stored primes lie in [X, Y]\n"
        "       %s FILE.db --next X       the smallest stored prime >= X, and its position\n"
        "       %s FILE.db --range X Y    print the stored primes in [X, Y], one per line\n"
        "       %s FILE.db --slice I J    print the primes at positions I..J, one per line\n"
        "       %s FILE.db --info         range, count, first/last prime, blocks, sizes\n"
        "\n"
        "Reads a .db (and its .blk sidecar) written by 'eratostenes N -o FILE.db'.\n"
        "Numbers take the same forms as eratostenes' N: 1e18, 100b, 1000000.\n"
        "Positions start at 1 with the first stored prime: 2 for a full run, the\n"
        "first prime >= S for a tail written with --start S (positions in a tail\n"
        "are relative to it). X and Y must lie inside the stored range.\n",
        prog, prog, prog, prog, prog, prog, prog);
}

struct Block {
    uint64_t start_index = 0; // position (0-based) of its first prime
    uint64_t count = 0;
    uint64_t start_prime = 0;
    uint64_t offset = 0; // in the .blk
    uint64_t len = 0;
};

class PrimeDb {
public:
    explicit PrimeDb(const std::string& path) : path_(path) {
        if (sqlite3_open_v2(path.c_str(), &db_, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
            const std::string msg = db_ ? sqlite3_errmsg(db_) : "out of memory";
            sqlite3_close(db_);
            db_ = nullptr;
            throw std::runtime_error("could not open " + path + ": " + msg);
        }
        version_ = meta("format_version");
        // 1 stored delta/2 gaps, 2 held the blocks as BLOBs inside the .db:
        // both need the eratostenes that wrote them. 3 is 4 without
        // range_start (always a full run).
        if (version_ != "3" && version_ != "4")
            throw std::runtime_error("unsupported .db format version " + version_ +
                                     " (expected 3 or 4): regenerate it with this eratostenes");
        total = std::strtoull(meta("total_primes").c_str(), nullptr, 10);
        limit = std::strtoull(meta("limit").c_str(), nullptr, 10);
        range_start = version_ == "3" ? 0 : std::strtoull(meta("range_start").c_str(), nullptr, 10);
        at_ = prepare("SELECT start_index, count, start_prime, offset, len FROM blocks "
                      "WHERE start_index <= ?1 ORDER BY start_index DESC LIMIT 1;");
    }
    PrimeDb(const PrimeDb&) = delete;
    PrimeDb& operator=(const PrimeDb&) = delete;
    ~PrimeDb() {
        if (at_) sqlite3_finalize(at_);
        if (blk_fd_ >= 0) ::close(blk_fd_);
        if (db_) sqlite3_close(db_);
    }

    uint64_t total = 0;       // stored primes
    uint64_t range_start = 0; // the S of --start S, 0 for a full run
    uint64_t limit = 0;       // N

    const std::string& version() const { return version_; }

    std::string meta(const char* key) {
        sqlite3_stmt* st = prepare("SELECT value FROM meta WHERE key = ?1;");
        sqlite3_bind_text(st, 1, key, -1, SQLITE_TRANSIENT);
        std::string v;
        const bool found = sqlite3_step(st) == SQLITE_ROW;
        if (found) {
            const unsigned char* t = sqlite3_column_text(st, 0);
            v = t ? reinterpret_cast<const char*>(t) : "";
        }
        sqlite3_finalize(st);
        if (!found) throw std::runtime_error(std::string("invalid .db file: missing meta.") + key);
        return v;
    }

    uint64_t block_count() {
        sqlite3_stmt* st = prepare("SELECT COUNT(*) FROM blocks;");
        sqlite3_step(st);
        const uint64_t n = static_cast<uint64_t>(sqlite3_column_int64(st, 0));
        sqlite3_finalize(st);
        return n;
    }

    // The block holding position pos (0-based, < total).
    Block block_at(uint64_t pos) {
        sqlite3_reset(at_);
        sqlite3_bind_int64(at_, 1, static_cast<sqlite3_int64>(pos));
        if (sqlite3_step(at_) != SQLITE_ROW) throw std::runtime_error("corrupt .db: no block holds position " + std::to_string(pos + 1));
        Block b = row(at_);
        if (pos >= b.start_index + b.count) throw std::runtime_error("corrupt .db: no block holds position " + std::to_string(pos + 1));
        return b;
    }

    // The block's primes, into out (resized to its count).
    void decode(const Block& b, std::vector<uint64_t>& out) {
        if (blk_fd_ < 0) open_blk();
        blob_.resize(b.len);
        size_t done = 0;
        while (done < b.len) {
            const ssize_t r = ::pread(blk_fd_, blob_.data() + done, b.len - done, static_cast<off_t>(b.offset + done));
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) throw std::runtime_error("short read from the block file: corrupt .db/.blk pair");
            done += static_cast<size_t>(r);
        }
        const unsigned long long raw_size = ZSTD_getFrameContentSize(blob_.data(), blob_.size());
        if (raw_size == ZSTD_CONTENTSIZE_ERROR || raw_size == ZSTD_CONTENTSIZE_UNKNOWN)
            throw std::runtime_error("corrupt block: unknown zstd size");
        raw_.resize(raw_size);
        const size_t got = ZSTD_decompressDCtx(dctx_.get(), raw_.data(), raw_.size(), blob_.data(), blob_.size());
        if (ZSTD_isError(got) || got != raw_size) throw std::runtime_error("corrupt block: zstd decompression failed");
        out.resize(b.count);
        if (!decode_block(raw_.data(), raw_.size(), b.start_prime, b.count, out.data()))
            throw std::runtime_error("corrupt block: its gaps don't match its prime count");
    }

    // The prime at position pos (0-based, < total).
    uint64_t at(uint64_t pos) {
        const Block b = block_at(pos);
        decode(b, buf_);
        return buf_[pos - b.start_index];
    }

    // How many stored primes are < v: a binary search over positions for the
    // last block whose first prime is < v (blocks partition [0, total) and
    // their first primes increase with it), then that block's own primes.
    uint64_t rank(uint64_t v) {
        uint64_t lo = 0, hi = total;
        bool found = false;
        Block cand;
        while (lo < hi) {
            const Block b = block_at(lo + (hi - lo) / 2);
            if (b.start_prime < v) { cand = b; found = true; lo = b.start_index + b.count; }
            else hi = b.start_index;
        }
        if (!found) return 0;
        decode(cand, buf_);
        return cand.start_index + static_cast<uint64_t>(std::lower_bound(buf_.begin(), buf_.end(), v) - buf_.begin());
    }

    // f(prime) for every position in [a, b) (0-based), in order, streaming
    // the blocks by start_index.
    template <typename F>
    void for_each(uint64_t a, uint64_t b, F&& f) {
        if (a >= b) return;
        const Block first = block_at(a);
        sqlite3_stmt* st = prepare("SELECT start_index, count, start_prime, offset, len FROM blocks "
                                   "WHERE start_index >= ?1 ORDER BY start_index;");
        sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(first.start_index));
        uint64_t expect = first.start_index;
        try {
            while (expect < b && sqlite3_step(st) == SQLITE_ROW) {
                const Block blk = row(st);
                if (blk.start_index != expect) throw std::runtime_error("corrupt .db: a gap in the block positions");
                decode(blk, buf_);
                const uint64_t from = std::max(a, blk.start_index), to = std::min(b, blk.start_index + blk.count);
                for (uint64_t i = from; i < to; ++i) f(buf_[i - blk.start_index]);
                expect = blk.start_index + blk.count;
            }
        } catch (...) {
            sqlite3_finalize(st);
            throw;
        }
        sqlite3_finalize(st);
        if (expect < b) throw std::runtime_error("corrupt .db: the blocks end before position " + std::to_string(b));
    }

    uint64_t blk_bytes() {
        return std::strtoull(meta("blk_bytes").c_str(), nullptr, 10);
    }

private:
    sqlite3_stmt* prepare(const char* sql) {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK)
            throw std::runtime_error(std::string("sqlite: ") + sqlite3_errmsg(db_));
        return st;
    }
    static Block row(sqlite3_stmt* st) {
        Block b;
        b.start_index = static_cast<uint64_t>(sqlite3_column_int64(st, 0));
        b.count = static_cast<uint64_t>(sqlite3_column_int64(st, 1));
        b.start_prime = static_cast<uint64_t>(sqlite3_column_int64(st, 2)); // bit pattern, also above 2^63
        b.offset = static_cast<uint64_t>(sqlite3_column_int64(st, 3));
        b.len = static_cast<uint64_t>(sqlite3_column_int64(st, 4));
        return b;
    }

    // The .blk named by meta.blk_file, next to the .db, checked against
    // meta.blk_bytes (a truncated or mismatched sidecar fails here, not with a
    // corrupt block later).
    void open_blk() {
        const std::string name = meta("blk_file");
        const size_t slash = path_.find_last_of('/');
        const std::string p = slash == std::string::npos ? name : path_.substr(0, slash + 1) + name;
        const int fd = ::open(p.c_str(), O_RDONLY);
        if (fd < 0) throw std::runtime_error("could not open the block file " + p + ": " + std::strerror(errno));
        struct stat sb{};
        if (::fstat(fd, &sb) != 0) { ::close(fd); throw std::runtime_error("fstat failed on " + p); }
        const uint64_t want = blk_bytes();
        if (static_cast<uint64_t>(sb.st_size) != want) {
            ::close(fd);
            throw std::runtime_error(p + " is " + std::to_string(sb.st_size) + " bytes, the .db expects " +
                                     std::to_string(want) + ": not the sidecar this .db was written with");
        }
        blk_fd_ = fd;
    }

    struct DctxFree { void operator()(ZSTD_DCtx* c) const { ZSTD_freeDCtx(c); } };

    std::string path_;
    std::string version_;
    sqlite3* db_ = nullptr;
    sqlite3_stmt* at_ = nullptr;
    int blk_fd_ = -1;
    std::unique_ptr<ZSTD_DCtx, DctxFree> dctx_{ZSTD_createDCtx()};
    std::vector<uint8_t> blob_, raw_;
    std::vector<uint64_t> buf_;
};

// Buffered stdout, one number per line.
class LineOut {
public:
    ~LineOut() { flush(); }
    void put(uint64_t v) {
        if (n_ + 21 > sizeof(buf_)) flush();
        n_ = static_cast<size_t>(std::to_chars(buf_ + n_, buf_ + sizeof(buf_), v).ptr - buf_);
        buf_[n_++] = '\n';
    }
    void flush() {
        if (n_ && std::fwrite(buf_, 1, n_, stdout) != n_) throw std::runtime_error("write to stdout failed");
        n_ = 0;
    }
private:
    char buf_[1 << 20];
    size_t n_ = 0;
};

static uint64_t parse_number(const char* what, const char* s) {
    if (s[0] == '-') throw std::runtime_error(std::string(what) + " must not be negative: " + s);
    return parse_size(s);
}

// X..Y must lie inside the stored range: a tail has no primes below its
// range_start, and nothing past limit was sieved, so an answer there would be
// silently partial.
static void check_values(const PrimeDb& db, uint64_t x, uint64_t y) {
    if (x < db.range_start || y > db.limit)
        throw std::runtime_error((x == y ? std::to_string(x) : std::to_string(x) + ".." + std::to_string(y)) +
                                 " is outside the stored range [" +
                                 std::to_string(db.range_start) + ", " + std::to_string(db.limit) + "]");
}

int main(int argc, char** argv) {
    if (argc >= 2 && (std::strcmp(argv[1], "-h") == 0 || std::strcmp(argv[1], "--help") == 0)) {
        print_nth_usage(argv[0]);
        return 0;
    }
    if (argc < 3) {
        print_nth_usage(argv[0]);
        return 1;
    }
    const std::string mode = argv[2];
    const int extra = argc - 3;
    const auto need = [&](int n) {
        if (extra != n) throw std::runtime_error(mode + " takes " + std::to_string(n) + " value(s)");
    };

    try {
        PrimeDb db(argv[1]);
        if (mode == "--count") {
            if (extra == 0) {
                std::printf("%llu\n", static_cast<unsigned long long>(db.total));
            } else {
                need(2);
                const uint64_t x = parse_number("X", argv[3]), y = parse_number("Y", argv[4]);
                if (x > y) { std::printf("0\n"); return 0; }
                check_values(db, x, y);
                std::printf("%llu\n", static_cast<unsigned long long>(db.rank(y + 1) - db.rank(x)));
            }
        } else if (mode == "--next") {
            need(1);
            const uint64_t x = parse_number("X", argv[3]);
            check_values(db, x, x);
            const uint64_t r = db.rank(x);
            if (r >= db.total)
                throw std::runtime_error("no stored prime >= " + std::to_string(x) + " (the range ends at " +
                                         std::to_string(db.limit) + ")");
            std::printf("%llu %llu\n", static_cast<unsigned long long>(db.at(r)), static_cast<unsigned long long>(r + 1));
        } else if (mode == "--range") {
            need(2);
            const uint64_t x = parse_number("X", argv[3]), y = parse_number("Y", argv[4]);
            if (x > y) return 0;
            check_values(db, x, y);
            LineOut out;
            db.for_each(db.rank(x), db.rank(y + 1), [&](uint64_t p) { out.put(p); });
        } else if (mode == "--slice") {
            need(2);
            const uint64_t i = parse_number("I", argv[3]), j = parse_number("J", argv[4]);
            if (i == 0) throw std::runtime_error("positions start at 1");
            if (i > j) return 0;
            if (j > db.total)
                throw std::runtime_error("position " + std::to_string(j) + " is past the last stored prime (" +
                                         std::to_string(db.total) + ")");
            LineOut out;
            db.for_each(i - 1, j, [&](uint64_t p) { out.put(p); });
        } else if (mode == "--info") {
            need(0);
            std::printf("format:       %s\n", db.version().c_str());
            std::printf("range:        [%s, %s]%s\n", format_thousands(db.range_start).c_str(),
                        format_thousands(db.limit).c_str(),
                        db.range_start ? " (a tail: positions count from its first prime)" : "");
            std::printf("primes:       %s\n", format_thousands(db.total).c_str());
            if (db.total) {
                std::printf("first prime:  %s\n", format_thousands(db.at(0)).c_str());
                std::printf("last prime:   %s\n", format_thousands(db.at(db.total - 1)).c_str());
            }
            const uint64_t blk = db.blk_bytes();
            std::printf("blocks:       %s of up to %s primes, zstd level %s\n", format_thousands(db.block_count()).c_str(),
                        format_thousands(std::strtoull(db.meta("block_size").c_str(), nullptr, 10)).c_str(),
                        db.meta("zstd_level").c_str());
            std::printf("block file:   %s (%s bytes, %.2f bits/prime)\n", db.meta("blk_file").c_str(),
                        format_thousands(blk).c_str(), db.total ? 8.0 * static_cast<double>(blk) / static_cast<double>(db.total) : 0.0);
        } else if (!mode.empty() && mode[0] == '-') {
            throw std::runtime_error("unknown option " + mode);
        } else {
            need(0);
            const uint64_t n = parse_number("N", argv[2]);
            if (n == 0) throw std::runtime_error("N must be >= 1 (positions start at 1)");
            if (n > db.total)
                throw std::runtime_error("index out of range: past the last stored prime (" + std::to_string(db.total) + ")");
            std::printf("%llu\n", static_cast<unsigned long long>(db.at(n - 1)));
        }
    } catch (const std::exception& e) {
        std::fflush(stdout);
        std::fprintf(stderr, "Error: %s\n", e.what());
        return 1;
    }
    return 0;
}
