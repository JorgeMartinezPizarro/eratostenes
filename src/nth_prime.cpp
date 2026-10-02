// Reads a .db produced by `eratostenes -o out.db` -- the SQLite index -- and
// its .blk sidecar (the gap-encoded, zstd-compressed blocks, one after the
// other; see gap_block_sink.hpp / block_file.hpp / sqlite_prime_store.hpp
// for the writer side, gap_encoding.hpp for the byte format shared by both)
// and prints the Nth prime, or the total count.
//
// Lookup: find the block whose start_index is the largest one <= the
// requested (0-based) index -- idx_blocks_start makes this an indexed
// query, not a table scan -- pread() just that block's bytes from the .blk
// at the row's (offset, len), decompress it and walk its gap stream up to
// the requested position: a few index pages, one contiguous read, one
// zstd frame, whatever the file's size.

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sqlite3.h>
#include <zstd.h>

#include "gap_encoding.hpp"

static void print_usage(const char* prog) {
    std::fprintf(stderr,
        "Usage: %s FILE.db N\n"
        "       %s FILE.db --count\n"
        "\n"
        "Prints the N-th prime (N starts at 1: N=1 -> 2) stored in FILE.db and\n"
        "its FILE.blk sidecar, generated with 'eratostenes -o FILE.db'.\n"
        "\n"
        "  --count    Print the number of stored primes (pi(limit)) and exit\n"
        "  -h, --help Show this help\n",
        prog, prog);
}

static std::string read_meta(sqlite3* db, const char* key) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT value FROM meta WHERE key = ?;", -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error(std::string("sqlite: ") + sqlite3_errmsg(db));
    }
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
    std::string result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char* text = sqlite3_column_text(stmt, 0);
        result = text ? reinterpret_cast<const char*>(text) : "";
    } else {
        sqlite3_finalize(stmt);
        throw std::runtime_error(std::string("invalid .db file: missing meta.") + key);
    }
    sqlite3_finalize(stmt);
    return result;
}

// Opens the .blk named by meta.blk_file, next to the .db, and checks its size
// against meta.blk_bytes (a truncated or mismatched sidecar fails here, not
// with a corrupt block later).
static int open_blk(sqlite3* db, const std::string& db_path) {
    const std::string name = read_meta(db, "blk_file");
    const size_t slash = db_path.find_last_of('/');
    const std::string path = slash == std::string::npos ? name : db_path.substr(0, slash + 1) + name;
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) throw std::runtime_error("could not open the block file " + path + ": " + std::strerror(errno));
    struct stat st{};
    if (::fstat(fd, &st) != 0) { ::close(fd); throw std::runtime_error("fstat failed on " + path); }
    const unsigned long long want = std::strtoull(read_meta(db, "blk_bytes").c_str(), nullptr, 10);
    if (static_cast<unsigned long long>(st.st_size) != want) {
        ::close(fd);
        throw std::runtime_error(path + " is " + std::to_string(st.st_size) + " bytes, the .db expects " +
                                 std::to_string(want) + ": not the sidecar this .db was written with");
    }
    return fd;
}

// target_index is 0-based (the Nth prime, 1-indexed at the CLI, is
// target_index = N - 1 here).
static uint64_t lookup(sqlite3* db, int blk_fd, uint64_t target_index) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT start_index, count, start_prime, offset, len FROM blocks "
        "WHERE start_index <= ?1 ORDER BY start_index DESC LIMIT 1;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error(std::string("sqlite: ") + sqlite3_errmsg(db));
    }
    sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(target_index));
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        throw std::runtime_error("index out of range: no block holds that position");
    }
    const uint64_t start_index = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
    const uint64_t count = static_cast<uint64_t>(sqlite3_column_int64(stmt, 1));
    const uint64_t start_prime = static_cast<uint64_t>(sqlite3_column_int64(stmt, 2));
    const uint64_t offset = static_cast<uint64_t>(sqlite3_column_int64(stmt, 3));
    const size_t len = static_cast<size_t>(sqlite3_column_int64(stmt, 4));
    sqlite3_finalize(stmt);

    if (target_index >= start_index + count) {
        throw std::runtime_error("index out of range: past the last generated prime");
    }

    std::vector<uint8_t> blob(len);
    size_t done = 0;
    while (done < len) {
        ssize_t r = ::pread(blk_fd, blob.data() + done, len - done, static_cast<off_t>(offset + done));
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) throw std::runtime_error("short read from the block file: corrupt .db/.blk pair");
        done += static_cast<size_t>(r);
    }

    unsigned long long raw_size = ZSTD_getFrameContentSize(blob.data(), blob.size());
    if (raw_size == ZSTD_CONTENTSIZE_ERROR || raw_size == ZSTD_CONTENTSIZE_UNKNOWN) {
        throw std::runtime_error("corrupt block: unknown zstd size");
    }
    std::vector<uint8_t> raw(raw_size);
    size_t decoded = ZSTD_decompress(raw.data(), raw.size(), blob.data(), blob.size());
    if (ZSTD_isError(decoded) || decoded != raw_size) {
        throw std::runtime_error("corrupt block: zstd decompression failed");
    }

    uint64_t value = start_prime;
    size_t pos = 0;
    const uint64_t steps = target_index - start_index;
    for (uint64_t i = 0; i < steps; ++i) {
        value = decode_gap(raw.data(), pos, value);
    }
    return value;
}

int main(int argc, char** argv) {
    if (argc >= 2 && (std::strcmp(argv[1], "-h") == 0 || std::strcmp(argv[1], "--help") == 0)) {
        print_usage(argv[0]);
        return 0;
    }
    if (argc != 3) {
        print_usage(argv[0]);
        return 1;
    }

    const char* path = argv[1];
    const char* arg2 = argv[2];

    sqlite3* db = nullptr;
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        std::fprintf(stderr, "Error: could not open %s: %s\n", path, sqlite3_errmsg(db));
        if (db) sqlite3_close(db);
        return 1;
    }

    int blk_fd = -1;
    try {
        // 1 stored delta/2 gaps, 2 held the blocks as BLOBs inside the .db:
        // both need the eratostenes that wrote them.
        std::string version = read_meta(db, "format_version");
        if (version != "3") {
            throw std::runtime_error("unsupported .db format version " + version +
                                     " (expected 3): regenerate it with this eratostenes");
        }
        if (std::strcmp(arg2, "--count") == 0) {
            std::string total = read_meta(db, "total_primes");
            std::printf("%s\n", total.c_str());
        } else {
            char* end = nullptr;
            unsigned long long n = std::strtoull(arg2, &end, 10);
            if (end == arg2 || *end != '\0' || n == 0) {
                throw std::runtime_error("N must be an integer >= 1");
            }
            blk_fd = open_blk(db, path);
            uint64_t value = lookup(db, blk_fd, n - 1);
            std::printf("%llu\n", static_cast<unsigned long long>(value));
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Error: %s\n", e.what());
        if (blk_fd >= 0) ::close(blk_fd);
        sqlite3_close(db);
        return 1;
    }

    if (blk_fd >= 0) ::close(blk_fd);
    sqlite3_close(db);
    return 0;
}
