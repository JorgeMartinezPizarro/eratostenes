#pragma once
// Owns the .db (SQLite) file for .db output mode: schema, a thread-safe
// queue of finished blocks (fed by every sieve thread's GapBlockSink via
// push()), and one dedicated writer thread that drains the queue with
// batched transactions. This keeps SQLite's single-writer constraint off
// the sieve/compress hot path -- that stays fully parallel across threads;
// only the (cheap, already-compressed) insert step is serialized.

#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <string>
#include <thread>
#include <sqlite3.h>

#include "gap_block_sink.hpp"

class SqlitePrimeStore {
public:
    explicit SqlitePrimeStore(const std::string& path) {
        // PRAGMA page_size only takes effect on a page-less (brand new)
        // database, so any stale file at this path must go first -- a 64KB
        // leftover page size would waste ~half a page per block via
        // internal fragmentation (this was the dominant overhead in the
        // prototype before it was tracked down).
        std::remove(path.c_str());

        check(sqlite3_open(path.c_str(), &db_), "open");
        exec("PRAGMA page_size=4096;");
        exec("PRAGMA journal_mode=WAL;");
        exec("PRAGMA synchronous=NORMAL;");
        exec("CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT);");
        exec("CREATE TABLE blocks ("
             "  block_id    INTEGER PRIMARY KEY,"
             "  start_index INTEGER NOT NULL,"
             "  count       INTEGER NOT NULL,"
             "  start_prime INTEGER NOT NULL,"
             "  data        BLOB NOT NULL"
             ");");
        check(sqlite3_prepare_v2(db_,
                  "INSERT INTO blocks (start_index, count, start_prime, data) VALUES (?,?,?,?);",
                  -1, &insert_stmt_, nullptr),
              "prepare insert");

        writer_ = std::thread([this] { writer_loop(); });
    }

    SqlitePrimeStore(const SqlitePrimeStore&) = delete;
    SqlitePrimeStore& operator=(const SqlitePrimeStore&) = delete;

    ~SqlitePrimeStore() {
        // Normal path is finish(); this only fires if an exception is
        // unwinding past us without finish() having run -- shut the writer
        // thread down cleanly (a joinable std::thread whose destructor
        // still finds it joinable calls std::terminate()) but swallow any
        // error, since we're already unwinding one.
        if (writer_.joinable()) {
            { std::lock_guard<std::mutex> lk(mu_); done_ = true; }
            cv_.notify_all();
            cv_not_full_.notify_all();
            writer_.join();
        }
        if (insert_stmt_) sqlite3_finalize(insert_stmt_);
        if (db_) sqlite3_close(db_);
    }

    // Called from sieve threads -- many concurrent callers, must stay safe.
    // Blocks (backpressure) once the queue holds too many not-yet-written
    // blocks: sieve+zstd across many threads can outrun the single
    // serialized SQLite writer, and an unbounded queue here means
    // compressed blocks pile up in RAM without limit -- for large N (e.g.
    // 1e12, hundreds of thousands of blocks) that's enough to OOM the
    // process before disk ever fills up.
    void push(PendingBlock blk) {
        std::unique_lock<std::mutex> lk(mu_);
        cv_not_full_.wait(lk, [&] { return queue_.size() < MAX_QUEUED_BLOCKS || done_; });
        queue_.push(std::move(blk));
        cv_.notify_one();
    }

    // Call once, after every sieve thread has joined. Drains the writer
    // thread, builds the lookup index (cheaper after bulk insert than
    // maintained incrementally), writes meta, and checkpoints WAL back into
    // a single clean file (no -wal/-shm sidecars) fit for shipping/copying.
    void finish(uint64_t total_primes, uint64_t limit, uint64_t wheel_mod,
                uint64_t block_size, int zstd_level) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            done_ = true;
        }
        cv_.notify_all();
        cv_not_full_.notify_all();
        writer_.join();
        if (writer_error_) std::rethrow_exception(writer_error_);

        sqlite3_finalize(insert_stmt_);
        insert_stmt_ = nullptr;

        exec("CREATE INDEX idx_blocks_start ON blocks(start_index);");

        write_meta("format_version", "1");
        write_meta("limit", std::to_string(limit));
        write_meta("wheel_mod", std::to_string(wheel_mod));
        write_meta("block_size", std::to_string(block_size));
        write_meta("zstd_level", std::to_string(zstd_level));
        write_meta("total_primes", std::to_string(total_primes));

        exec("PRAGMA wal_checkpoint(TRUNCATE);");
        exec("PRAGMA journal_mode=DELETE;");
    }

private:
    void check(int rc, const char* what) {
        if (rc != SQLITE_OK) {
            std::string msg = std::string("sqlite ") + what + ": " +
                               (db_ ? sqlite3_errmsg(db_) : "no se pudo abrir");
            throw std::runtime_error(msg);
        }
    }

    void exec(const char* sql) {
        char* err = nullptr;
        if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
            std::string msg = err ? err : "error desconocido";
            sqlite3_free(err);
            throw std::runtime_error("sqlite exec fallo (" + std::string(sql) + "): " + msg);
        }
    }

    void write_meta(const std::string& key, const std::string& value) {
        sqlite3_stmt* stmt = nullptr;
        check(sqlite3_prepare_v2(db_, "INSERT INTO meta(key,value) VALUES(?,?);", -1, &stmt, nullptr),
              "prepare meta");
        sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, value.c_str(), -1, SQLITE_TRANSIENT);
        int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (rc != SQLITE_DONE) throw std::runtime_error("sqlite: fallo insertando meta." + key);
    }

    void writer_loop() {
        try {
            const size_t BATCH = 1000;
            size_t in_txn = 0;
            bool txn_open = false;
            for (;;) {
                PendingBlock blk;
                {
                    std::unique_lock<std::mutex> lk(mu_);
                    cv_.wait(lk, [&] { return !queue_.empty() || done_; });
                    if (queue_.empty() && done_) break;
                    blk = std::move(queue_.front());
                    queue_.pop();
                    cv_not_full_.notify_one();
                }
                if (!txn_open) { exec("BEGIN;"); txn_open = true; in_txn = 0; }
                insert_block(blk);
                if (++in_txn >= BATCH) { exec("COMMIT;"); txn_open = false; }
            }
            if (txn_open) exec("COMMIT;");
        } catch (...) {
            writer_error_ = std::current_exception();
        }
    }

    void insert_block(const PendingBlock& blk) {
        sqlite3_reset(insert_stmt_);
        sqlite3_bind_int64(insert_stmt_, 1, static_cast<sqlite3_int64>(blk.start_index));
        sqlite3_bind_int64(insert_stmt_, 2, static_cast<sqlite3_int64>(blk.count));
        sqlite3_bind_int64(insert_stmt_, 3, static_cast<sqlite3_int64>(blk.start_prime));
        sqlite3_bind_blob(insert_stmt_, 4, blk.compressed.data(),
                           static_cast<int>(blk.compressed.size()), SQLITE_TRANSIENT);
        if (sqlite3_step(insert_stmt_) != SQLITE_DONE) {
            throw std::runtime_error(std::string("sqlite: fallo insertando bloque: ") + sqlite3_errmsg(db_));
        }
    }

    sqlite3* db_ = nullptr;
    sqlite3_stmt* insert_stmt_ = nullptr;

    // Caps how many compressed-but-not-yet-inserted blocks can queue up.
    // Generous enough to smooth out scheduling jitter between producer
    // threads and the single writer, but bounded so a writer that's
    // slower than the sieve+compress side (large N, slow disk, many
    // threads) applies backpressure instead of growing the queue --
    // and process memory -- without limit.
    static constexpr size_t MAX_QUEUED_BLOCKS = 512;

    std::mutex mu_;
    std::condition_variable cv_;
    std::condition_variable cv_not_full_;
    std::queue<PendingBlock> queue_;
    bool done_ = false;

    std::thread writer_;
    std::exception_ptr writer_error_;
};
