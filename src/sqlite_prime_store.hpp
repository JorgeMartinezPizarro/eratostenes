#pragma once
// Owns the .db (SQLite) file for .db output mode -- the index: schema, a
// thread-safe queue of finished blocks' index rows (fed by every sieve
// thread's GapBlockSink via push()), and one dedicated writer thread that
// drains the queue with batched transactions -- plus the .blk sidecar
// (block_file.hpp) the compressed blocks themselves go to, written by the
// sieve threads in parallel. SQLite's single writer only ever sees ~40-byte
// rows (the blocks used to go through it as BLOBs: docs/RESEARCH.md).
//
// No separate counting pre-pass feeds this any more (see gap_block_sink.hpp
// and main.cpp's run_db): every block arrives with a
// chunk-relative start_index and its chunk_id, and finish() corrects
// start_index up to the true global offset with one UPDATE per chunk
// (fix_offsets, called from finish before the start_index index is built)
// once every chunk's real prime count -- a byproduct of the single sieve
// pass, not a second one -- is known.

#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <exception>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <string>
#include <thread>
#include <sqlite3.h>

#include "block_file.hpp"
#include "gap_block_sink.hpp"

class SqlitePrimeStore {
public:
    explicit SqlitePrimeStore(const std::string& path) : blocks_(blk_path_for(path)) {
        // PRAGMA page_size only takes effect on a page-less (brand new)
        // database, so any stale file at this path must go first. See
        // docs/RESEARCH.md#page-size-kept.
        std::remove(path.c_str());

        check(sqlite3_open(path.c_str(), &db_), "open");
        exec("PRAGMA page_size=4096;");
        // WAL, not journal_mode=OFF: OFF writes every page once instead of
        // twice, and won 2-3x on the dev PC at 1e11, but lost clearly on the
        // server at 1e12 (A/B, sync included) -- in-place page writes scatter
        // where WAL appends. See docs/RESEARCH.md.
        exec("PRAGMA journal_mode=WAL;");
        exec("PRAGMA synchronous=NORMAL;");
        // A bigger PRAGMA cache_size (512MB, up from SQLite's own default
        // 2MB) was tried and measured worse on the server at both N=1e12
        // and N=1e13 -- see docs/RESEARCH.md.
        exec("CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT);");
        // One row per block: where it sits in the prime sequence
        // (start_index, corrected after the fact -- see fix_offsets -- and
        // count), its first prime, and where its compressed bytes sit in the
        // .blk file (offset, len).
        exec("CREATE TABLE blocks ("
             "  block_id    INTEGER PRIMARY KEY,"
             "  chunk_id    INTEGER NOT NULL,"
             "  start_index INTEGER NOT NULL,"
             "  count       INTEGER NOT NULL,"
             "  start_prime INTEGER NOT NULL,"
             "  offset      INTEGER NOT NULL,"
             "  len         INTEGER NOT NULL"
             ");");
        check(sqlite3_prepare_v2(db_,
                  "INSERT INTO blocks (chunk_id, start_index, count, start_prime, offset, len) VALUES (?,?,?,?,?,?);",
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

    // The .blk file the sinks write their compressed blocks to.
    BlockFile& block_file() { return blocks_; }

    // Called from sieve threads -- many concurrent callers, must stay safe.
    // Blocks (backpressure) once the queue holds too many not-yet-inserted
    // rows, so a writer slower than the producers bounds memory instead of
    // growing the queue without limit.
    void push(PendingBlock blk) {
        std::unique_lock<std::mutex> lk(mu_);
        cv_not_full_.wait(lk, [&] { return queue_.size() < MAX_QUEUED_BLOCKS || done_; });
        queue_.push(std::move(blk));
        cv_.notify_one();
    }

    // Call once, after every sieve thread has joined. Drains the writer
    // thread, corrects every block's start_index from chunk-relative to
    // global (see fix_offsets), builds the lookup index (cheaper after bulk
    // insert/fixup than maintained incrementally), writes meta, and
    // checkpoints WAL back into a single clean file (no -wal/-shm sidecars)
    // fit for shipping/copying.
    //
    // chunk_offset[i]: how many primes precede chunk i globally (a prefix
    // sum over each chunk's real count, computed by the caller once every
    // chunk has finished sieving -- see main.cpp's run_db).
    // chunk_offset[0] is always 0 by construction. Empty is fine (e.g.
    // write_tiny_db's single implicit chunk 0, already at the right
    // offset) -- fix_offsets then has nothing to correct.
    // range_start: the S of --start S (0 for a full run). Positions
    // (start_index) count from the first prime >= S, so a tail's are
    // relative to it -- see format_version below.
    void finish(uint64_t total_primes, uint64_t range_start, uint64_t limit, uint64_t wheel_mod,
                uint64_t block_size, int zstd_level,
                const std::vector<uint64_t>& chunk_offset) {
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

        const uint64_t blk_bytes = blocks_.finish();

        fix_offsets(chunk_offset);

        exec("CREATE INDEX idx_blocks_start ON blocks(start_index);");

        // 4: range_start, the first number of the range the primes were
        // sieved from (0 for [0, limit]); positions count from the first
        // prime >= range_start. 3: blocks in the .blk sidecar (2 held them as
        // BLOBs; the gap encoding is version 2's, see gap_encoding.hpp) -- a
        // 3 is a 4 with range_start 0. blk_file is the sidecar's name next to
        // the .db, blk_bytes its size: nth_prime checks both.
        write_meta("format_version", "4");
        write_meta("range_start", std::to_string(range_start));
        {
            const std::string& bp = blocks_.path();
            const size_t slash = bp.find_last_of('/');
            write_meta("blk_file", slash == std::string::npos ? bp : bp.substr(slash + 1));
        }
        write_meta("blk_bytes", std::to_string(blk_bytes));
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
                               (db_ ? sqlite3_errmsg(db_) : "could not open");
            throw std::runtime_error(msg);
        }
    }

    void exec(const char* sql) {
        char* err = nullptr;
        if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
            std::string msg = err ? err : "unknown error";
            sqlite3_free(err);
            throw std::runtime_error("sqlite exec failed (" + std::string(sql) + "): " + msg);
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
        if (rc != SQLITE_DONE) throw std::runtime_error("sqlite: failed inserting meta." + key);
    }

    void writer_loop() {
        try {
            // Commits-per-transaction; 1000 measured best against
            // 3000/10000/100000. See
            // docs/RESEARCH.md#write-pipeline-knobs-batch---db-block-size-wal_autocheckpoint-all-measured-kept-at-their-defaults.
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
        sqlite3_bind_int64(insert_stmt_, 1, static_cast<sqlite3_int64>(blk.chunk_id));
        sqlite3_bind_int64(insert_stmt_, 2, static_cast<sqlite3_int64>(blk.start_index));
        sqlite3_bind_int64(insert_stmt_, 3, static_cast<sqlite3_int64>(blk.count));
        sqlite3_bind_int64(insert_stmt_, 4, static_cast<sqlite3_int64>(blk.start_prime));
        sqlite3_bind_int64(insert_stmt_, 5, static_cast<sqlite3_int64>(blk.offset));
        sqlite3_bind_int64(insert_stmt_, 6, static_cast<sqlite3_int64>(blk.len));
        if (sqlite3_step(insert_stmt_) != SQLITE_DONE) {
            throw std::runtime_error(std::string("sqlite: failed inserting block: ") + sqlite3_errmsg(db_));
        }
    }

    // Adds each chunk's global offset to its blocks' (still chunk-relative)
    // start_index -- a handful of UPDATEs (one per non-zero-offset chunk),
    // not one per block. Blocks from different chunks land interleaved in
    // insertion order (many sieve threads racing into one queue), so a
    // temporary index on chunk_id is what keeps each UPDATE's WHERE an
    // indexed lookup instead of a full table scan; dropped again right
    // after since only idx_blocks_start (built next, in finish()) is meant
    // to outlive this function.
    void fix_offsets(const std::vector<uint64_t>& chunk_offset) {
        bool any = false;
        for (uint64_t off : chunk_offset) if (off != 0) { any = true; break; }
        if (!any) return;

        exec("CREATE INDEX idx_blocks_chunk ON blocks(chunk_id);");
        sqlite3_stmt* stmt = nullptr;
        check(sqlite3_prepare_v2(db_,
                  "UPDATE blocks SET start_index = start_index + ?1 WHERE chunk_id = ?2;",
                  -1, &stmt, nullptr),
              "prepare offset fixup");
        exec("BEGIN;");
        for (size_t i = 0; i < chunk_offset.size(); ++i) {
            if (chunk_offset[i] == 0) continue; // chunk already at the right offset
            sqlite3_reset(stmt);
            sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(chunk_offset[i]));
            sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(i));
            if (sqlite3_step(stmt) != SQLITE_DONE) {
                sqlite3_finalize(stmt);
                throw std::runtime_error(std::string("sqlite: failed fixing start_index: ") + sqlite3_errmsg(db_));
            }
        }
        exec("COMMIT;");
        sqlite3_finalize(stmt);
        exec("DROP INDEX idx_blocks_chunk;");
    }

    sqlite3* db_ = nullptr;
    sqlite3_stmt* insert_stmt_ = nullptr;
    BlockFile blocks_;

    // Caps how many not-yet-inserted index rows can queue up (~40 bytes
    // each, so this bounds the wait, not memory).
    static constexpr size_t MAX_QUEUED_BLOCKS = 4096;

    std::mutex mu_;
    std::condition_variable cv_;
    std::condition_variable cv_not_full_;
    std::queue<PendingBlock> queue_;
    bool done_ = false;

    std::thread writer_;
    std::exception_ptr writer_error_;
};
