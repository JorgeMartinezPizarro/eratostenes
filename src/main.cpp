// Segmented, parallel, bit-packed Sieve of Eratosthenes, on a wheel that
// skips multiples of a small fixed set of primes up front (WHEEL_PRIMES in
// wheel.hpp -- see that file for why bigger isn't always better here, and
// why it's a compile-time constant instead of a CLI flag).
//
// Strategy:
//   1. Compute the base primes (<= sqrt(N)) with a simple sieve.
//   2. The "wheel index" range [1, wheel_count_upto(N)) -- which enumerates
//      the numbers coprime with WHEEL_MOD above the wheel's own primes, see
//      wheel.hpp -- is split into many more contiguous chunks than threads;
//      threads pull chunks from a shared queue instead of owning one each
//      (see run_parallel_chunks for why: work isn't uniform across chunks).
//   3. Text output (-o *.txt) needs two passes, because
//      pwrite() needs an exact byte OFFSET per thread up front:
//        a. COUNT PASS: each thread sieves its chunk and counts how many
//           bytes of text its primes will take (writes nothing to disk).
//           Prefix sums over those totals give the exact offset where each
//           thread must start writing in the final file.
//        b. The final file is resized to its exact, already-known size.
//        c. WRITE PASS: each thread re-sieves its chunk (same work) and
//           this time writes with pwrite() directly into its (disjoint)
//           region of the final file, in parallel with every other thread.
//      This avoids the "write to temp files + merge" pattern, which
//      doubles disk I/O (every output byte gets written twice). Here every
//      byte of the final result is written exactly once; the extra cost is
//      repeating the bit-marking phase (cheap, CPU/cache-bound) instead of
//      repeating a disk-to-disk copy (expensive, I/O-bound).
//   4. .db output (-o *.db) needs only ONE pass: blocks go through an
//      async queue to a single writer thread (SqlitePrimeStore), not to a
//      precomputed file offset, so there is nothing a second pass would
//      need to have precomputed -- each chunk's real prime count (needed
//      only for each block's start_index, itself just a metadata column
//      used to find a block later, see nth_prime.cpp) falls out of this
//      same pass for free. See gap_block_sink.hpp and
//      SqlitePrimeStore::finish/fix_offsets for how start_index gets
//      corrected from chunk-relative to global after the fact.
//
// No -o (the default) skips the write/emit pass (and the file) entirely: a
// single pass (like .db's) already yields the total, so nothing else needs
// to run.
//
// The wheel's own primes (WHEEL_PRIMES) are special-cased in thread 0 --
// they don't take part in the wheel numbering, so they're just emitted
// directly instead of being found by sieving.

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <fcntl.h>
#include <unistd.h>

#include "arg_parser.hpp"
#include "base_sieve.hpp"
#include "colors.hpp"
#include "gap_block_sink.hpp"
#include "presieve.hpp"
#include "segment_sieve.hpp"
#include "sinks.hpp"
#include "sqlite_prime_store.hpp"
#include "tuning.hpp"
#include "wheel.hpp"

namespace fs = std::filesystem;

// Dispatches -o/--output on its extension: ".db" (case-insensitive, the same
// test blk_path_for applies) means the SQLite + zstd gap-encoded format
// (gap_block_sink.hpp, sqlite_prime_store.hpp); anything else keeps the
// original one-prime-per-line text format.
static bool is_db_output(const std::string& path) { return has_db_suffix(path); }

// Writes a (small, already-known) list of primes straight into a .db store,
// with no threading -- used by the tiny-N early-return paths below, where
// N is too small for the parallel wheel machinery to apply at all.
static void write_tiny_db(const std::string& path, const std::vector<uint64_t>& primes,
                           uint64_t limit, uint64_t block_size, int zstd_level) {
    SqlitePrimeStore store(path);
    {
        GapBlockSink sink(0, block_size, zstd_level, store.block_file(),
                           [&store](PendingBlock b) { store.push(std::move(b)); });
        for (uint64_t p : primes) sink.write_uint64(p);
        sink.flush();
    }
    store.finish(primes.size(), limit, WHEEL_MOD, block_size, zstd_level, {});
}

// Text for the wheel's own primes (e.g. "2\n3\n5\n" for a mod-30 wheel),
// built once from WHEEL_PRIMES so it never needs to be kept in sync by hand.
inline std::string build_small_primes_text() {
    std::string s;
    for (uint64_t p : WHEEL_PRIMES) {
        s += std::to_string(p);
        s += '\n';
    }
    return s;
}
const std::string SMALL_PRIMES_TEXT = build_small_primes_text();
const uint64_t SMALL_PRIMES_BYTES = SMALL_PRIMES_TEXT.size();
const uint64_t SMALL_PRIMES_COUNT = WHEEL_PRIMES.size();

// What sieve_chunk measured on its last chunk, for run_parallel_chunks'
// steal decisions (read back on the same thread right after the chunk).
struct ChunkStats {
    double activate_s = 0;  // fresh start: time to activate the base primes
    uint64_t activated = 0; // how many it activated (0: carried on, nothing timed)
    double sieve_s = 0;     // the rest of the chunk
    uint64_t k = 0;         // wheel indices in the chunk
};
static thread_local ChunkStats t_chunk_stats;

// Splits the wheel indices into `chunks` chunks as evenly as possible, each
// (but the last) a whole number of `align` indices -- the segment width, so a
// worker can carry its sieve from one chunk into the next (sieve_chunk).
// k=0 is the number 1 (not prime; SegmentSieve clears it itself);
// k_end_exclusive is wheel_count_upto(limit), the first index whose number
// exceeds limit.
static std::vector<ChunkRange> split_ranges(uint64_t limit, unsigned chunks, uint64_t start, uint64_t align) {
    std::vector<ChunkRange> ranges;
    // Starts at k=0 (the number 1, cleared by SegmentSieve itself) rather
    // than k=1, and every chunk boundary is a multiple of 64: the
    // byte-addressed dense tiers (erat_small.hpp) need each segment to
    // start on a word boundary. `start` > 0 (--start, see main)
    // sieves only the tail [start, limit], rounded down to that boundary.
    // wheel_count_upto(start - 1) is the index of the first number >= start
    // (the same index SieveConfig::skip_below_k holds); wheel_count_upto(start)
    // is one past it when start is on the wheel, which left `start` itself
    // out whenever its index was 63 mod 64 (239, 2399, 4799: one prime
    // short of primesieve, found 2026-10-05).
    uint64_t k_start = start ? wheel_count_upto(start - 1) / 64 * 64 : 0;
    uint64_t k_end = wheel_count_upto(limit);
    if (k_end <= k_start) return ranges;

    uint64_t total = k_end - k_start;
    uint64_t per_chunk = (total + chunks - 1) / chunks;
    per_chunk = (per_chunk + align - 1) / align * align; // align: a multiple of 64

    uint64_t cursor = k_start;
    uint64_t remaining = total;
    for (unsigned t = 0; t < chunks && remaining > 0; ++t) {
        uint64_t take = std::min(per_chunk, remaining);
        uint64_t low = cursor;
        uint64_t high = low + take; // exclusive
        ranges.push_back({low, high});
        cursor = high;
        remaining -= take;
    }
    return ranges;
}

// Runs a chunk through SegmentSieve, segment by segment, feeding every
// found prime to 'out'. Shared by every pass (count-only, byte-counting,
// writing) -- they only differ in which Writer they pass in.
//
// One SegmentSieve per thread and tier set, reused across that thread's
// chunks (begin_chunk() resets every piece of per-chunk state; the segment
// buffer needs no clearing, the presieve fill overwrites it), instead of
// one per chunk: each construction allocated the 768 med64 lists and the
// segment buffer again. Worker threads only live for one pass
// (run_parallel_chunks), so the cache goes away with them.
//
// A chunk that starts where the thread's previous one on the same tiers
// ended (run_parallel_chunks hands out contiguous runs) carries on from that
// sieve state as if both were one chunk: no begin_chunk(), no activation of
// every base prime again -- at the top of 1e18 that is 50M primes, ~1.3s per
// thread. Only valid after a chunk of whole segments (split_ranges aligns all
// but the last), since the sparse ring counts in whole segments.
template <typename Writer>
static void sieve_chunk(ChunkRange range, const TierSet& t, uint64_t base_prime_max,
                         const Presieve& presieve, const SieveConfig& cfg, Writer& out, uint64_t& local_count,
                         std::atomic<uint64_t>& progress) {
    struct Slot {
        const TierSet* tiers = nullptr;
        std::unique_ptr<SegmentSieve> sieve;
        uint64_t next_k = UINT64_MAX; // where the sieve's state stands, if a chunk can go on from it
    };
    thread_local Slot slots[2]; // a run has at most two tier sets (narrow, wide)
    Slot* slot = slots[0].tiers == &t ? &slots[0]
               : slots[1].tiers == &t ? &slots[1]
               : slots[0].tiers == nullptr ? &slots[0] : &slots[1];
    if (slot->tiers != &t) {
        slot->sieve = std::make_unique<SegmentSieve>(t.width, base_prime_max, presieve, cfg.sub_block_bytes,
                                                     !t.sparse.empty(),
                                                     t.medium.size() >= cfg.medium_nta_min_primes, cfg.huge_arenas);
        slot->sieve->set_skip_below_k(cfg.skip_below_k);
        slot->tiers = &t;
        slot->next_k = UINT64_MAX;
    }
    SegmentSieve& sieve = *slot->sieve;
    ChunkStats& st = t_chunk_stats;
    st = {};
    auto t0 = std::chrono::steady_clock::now();
    if (slot->next_k != range.low) {
        // Fresh start: activate up front (the first sieve_and_emit would do
        // it anyway) so it can be timed apart from the sieving.
        sieve.begin_chunk();
        st.activated = sieve.activate(range.low, std::min(range.low + t.width, range.high),
                                      t.small, t.med64, t.medium, t.sparse);
        const auto t1 = std::chrono::steady_clock::now();
        st.activate_s = std::chrono::duration<double>(t1 - t0).count();
        t0 = t1;
    }
    slot->next_k = UINT64_MAX; // until this chunk is done (an exception leaves it unusable)
    for (uint64_t k_low = range.low; k_low < range.high; k_low += t.width) {
        uint64_t k_high = std::min(k_low + t.width, range.high);
        sieve.sieve_and_emit(k_low, k_high, t.small, t.med64, t.medium, t.sparse, out, local_count);
        progress.fetch_add(k_high - k_low, std::memory_order_relaxed);
    }
    st.sieve_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    st.k = range.high - range.low;
    if ((range.high - range.low) % t.width == 0) slot->next_k = range.high;
}

// Count-only pass: no I/O, no byte accounting, just the prime count.
static void count_only_worker(ChunkRange range, const TierSet& t, uint64_t base_prime_max,
                               const Presieve& presieve, const SieveConfig& cfg,
                               uint64_t& out_count, std::atomic<uint64_t>& progress) {
    NullSink sink;
    uint64_t local_count = 0;
    sieve_chunk(range, t, base_prime_max, presieve, cfg, sink, local_count, progress);
    out_count = local_count;
}

// Byte-counting pass: no disk I/O, just measures how many text bytes each
// thread's primes will take.
static void count_worker(ChunkRange range, const TierSet& t, uint64_t base_prime_max,
                          const Presieve& presieve, const SieveConfig& cfg,
                          uint64_t& out_bytes, uint64_t& out_count,
                          std::atomic<uint64_t>& progress) {
    ByteCounter counter;
    uint64_t local_count = 0;
    sieve_chunk(range, t, base_prime_max, presieve, cfg, counter, local_count, progress);
    out_bytes = counter.total_bytes;
    out_count = local_count;
}

// Write pass: re-sieves the same chunk and writes with pwrite() directly
// into its (disjoint) region of the final file.
static void emit_worker(int idx, ChunkRange range, const TierSet& t, uint64_t base_prime_max,
                         const Presieve& presieve, const SieveConfig& cfg,
                         int fd, uint64_t base_offset,
                         std::atomic<uint64_t>& progress) {
    DirectWriter out(fd, base_offset);
    uint64_t local_count = 0;

    if (idx == 0) {
        out.write_raw(SMALL_PRIMES_TEXT.data(), SMALL_PRIMES_BYTES);
    }

    sieve_chunk(range, t, base_prime_max, presieve, cfg, out, local_count, progress);
    out.flush();
}

// .db pass: sieves the chunk once and feeds a GapBlockSink, which
// gap-encodes/compresses in BLOCK_SIZE-prime blocks and pushes each one to
// 'store' (thread-safe, see SqlitePrimeStore::push) as it completes -- no
// disjoint-region bookkeeping needed here, unlike DirectWriter/pwrite, and
// (unlike before) no separate counting pre-pass either: the sink writes a
// chunk-relative start_index (SqlitePrimeStore::finish fixes it up to the
// true global offset afterwards, from out_count -- see main()'s
// is_db_output block), and SQLite doesn't care what order rows are
// inserted in either way.
static void emit_db_worker(int idx, ChunkRange range, const TierSet& t, uint64_t base_prime_max,
                            const Presieve& presieve, const SieveConfig& cfg,
                            SqlitePrimeStore& store,
                            uint64_t block_size, int zstd_level,
                            uint64_t& out_count,
                            std::atomic<uint64_t>& progress) {
    GapBlockSink sink(static_cast<uint64_t>(idx), block_size, zstd_level, store.block_file(),
                       [&store](PendingBlock b) { store.push(std::move(b)); });
    uint64_t local_count = 0;

    if (idx == 0) {
        for (uint64_t p : WHEEL_PRIMES) sink.write_uint64(p);
    }

    sieve_chunk(range, t, base_prime_max, presieve, cfg, sink, local_count, progress);
    sink.flush();
    out_count = local_count;
}

// Wakes the progress thread as soon as ProgressGuard sets `done`, instead
// of it finishing out a 500ms sleep: with a plain sleep_for, joining it
// delayed every pass's end by up to 500ms, which quantized the reported
// "total:" time to the next 0.5s tick (1e8..1e10 all read 0.51s). Only one
// progress thread is ever alive at a time, so one shared pair is enough.
static std::mutex g_progress_mu;
static std::condition_variable g_progress_cv;

static void print_progress(const Colors& C, const char* label, std::atomic<uint64_t>& progress,
                            uint64_t total, std::atomic<bool>& done) {
    using namespace std::chrono_literals;
    while (!done.load()) {
        {
            std::unique_lock<std::mutex> lk(g_progress_mu);
            g_progress_cv.wait_for(lk, 500ms, [&] { return done.load(); });
        }
        if (done.load()) break;
        uint64_t d = progress.load(std::memory_order_relaxed);
        double pct = total ? std::min(100.0, 100.0 * d / total) : 100.0;
        std::fprintf(stderr, "\r  %s%s:%s %s%5.1f%%%s   ", C.label, label, C.reset, C.time, pct, C.reset);
        std::fflush(stderr);
    }
    std::fprintf(stderr, "\r  %s%s:%s %s100.0%%%s   \n", C.label, label, C.reset, C.time, C.reset);
}

// Stops the progress thread on scope exit, normal or exceptional -- a
// joinable std::thread whose destructor runs while still joinable calls
// std::terminate(), so a worker exception unwinding past `prog` without
// this would crash before ever reaching the catch in main().
struct ProgressGuard {
    std::atomic<bool>& done;
    std::thread& th;
    ~ProgressGuard() {
        {
            // Under the mutex, so the store can't land between the
            // waiter's predicate check and its going to sleep.
            std::lock_guard<std::mutex> lk(g_progress_mu);
            done = true;
        }
        g_progress_cv.notify_all();
        th.join();
    }
};

// Spawns 'workers' OS threads over the chunks in `ranges`, calls fn(i) once
// for every chunk index, then joins them all and rethrows the first exception
// any of them raised (plain std::thread can't propagate one on its own).
//
// Each worker starts on its own contiguous run of chunks (an equal share of
// the indices) and walks it in order, so sieve_chunk carries its sieve from
// one chunk into the next instead of activating every base prime per chunk.
// A worker whose run is empty steals the back of another run, paying one
// activation for the whole piece. Many more chunks than workers keep the
// steals fine-grained: chunks aren't equal-work (see ALGORITHM.md §4) and
// neither are cores (P/E, SMT siblings). See
// docs/RESEARCH.md#run_parallel_chunks-chunk-granularity-idle-time-investigation-2026-09-25-external-review-opus-55
// for CHUNKS_PER_THREAD=150 (tuned with a plain shared-counter queue).
//
// Steals are priced with what the run itself has measured (sieve_chunk's
// ChunkStats, no extra work): each worker's sieving rate (wheel indices per
// second) and the time to activate one base prime, pooled over every fresh
// start so far. The victim is the run whose own worker would take the
// longest to finish it; the thief takes the back piece, in whole chunks,
// that has both finish together -- activation + piece / thief's rate =
// (left - piece) / victim's rate -- so a fast core takes more than half
// from a slow one, and nothing when the piece doesn't pay its activation.
// fresh_primes[i]: how many base primes a fresh start at chunk i activates.
// Until a worker has finished a chunk, the fallback is the back half of the
// longest run if it spans cfg.steal_min_k indices.
template <typename Fn>
static void run_parallel_chunks(unsigned workers, const std::vector<ChunkRange>& ranges,
                                const std::vector<uint64_t>& fresh_primes, const SieveConfig& cfg, Fn&& fn) {
    const unsigned num_chunks = static_cast<unsigned>(ranges.size());
    struct Run { unsigned next, end; };
    std::vector<Run> runs(workers);
    for (unsigned w = 0; w < workers; ++w)
        runs[w] = {static_cast<unsigned>(uint64_t{num_chunks} * w / workers),
                   static_cast<unsigned>(uint64_t{num_chunks} * (w + 1) / workers)};
    std::mutex runs_mu; // one lock per chunk taken: chunks take milliseconds at least
    unsigned steals = 0;
    std::vector<double> sieve_s(workers, 0.0); // measured so far, under runs_mu
    std::vector<uint64_t> sieved_k(workers, 0);
    double activate_s = 0;
    uint64_t activated = 0;
    auto rate = [&](unsigned v) { return sieve_s[v] > 0 ? static_cast<double>(sieved_k[v]) / sieve_s[v] : 0.0; };
    auto left_k = [&](const Run& r) { return static_cast<double>(ranges[r.end - 1].high - ranges[r.next].low); };
    // Next chunk index for worker w (done: what its last chunk measured), or
    // num_chunks when it should stop.
    auto take = [&](unsigned w, const ChunkStats* done) -> unsigned {
        std::lock_guard<std::mutex> lk(runs_mu);
        if (done) {
            sieve_s[w] += done->sieve_s;
            sieved_k[w] += done->k;
            activate_s += done->activate_s;
            activated += done->activated;
        }
        Run& own = runs[w];
        if (own.next == own.end) {
            unsigned victim = workers, mid = 0;
            const double r_t = rate(w);
            if (r_t > 0 && activated > 0) {
                double longest = 0;
                for (unsigned v = 0; v < workers; ++v) {
                    if (runs[v].end - runs[v].next < 2) continue; // its owner keeps the next chunk
                    const double t_v = left_k(runs[v]) / (rate(v) > 0 ? rate(v) : r_t);
                    if (t_v > longest) { longest = t_v; victim = v; }
                }
                if (victim == workers) return num_chunks;
                const Run& vr = runs[victim];
                const double r_v = rate(victim) > 0 ? rate(victim) : r_t;
                const double act = activate_s / static_cast<double>(activated) * static_cast<double>(fresh_primes[vr.end - 1]);
                const double piece = (left_k(vr) / r_v - act) / (1 / r_t + 1 / r_v);
                mid = vr.end;
                while (mid - 1 > vr.next && static_cast<double>(ranges[vr.end - 1].high - ranges[mid - 1].low) <= piece) --mid;
                if (mid == vr.end) return num_chunks; // not even one chunk pays its activation
            } else {
                unsigned left = 1;
                for (unsigned v = 0; v < workers; ++v)
                    if (runs[v].end - runs[v].next > left) { victim = v; left = runs[v].end - runs[v].next; }
                if (victim == workers) return num_chunks; // no run with 2+ chunks left
                mid = runs[victim].end - left / 2;
                if (ranges[runs[victim].end - 1].high - ranges[mid].low < cfg.steal_min_k) return num_chunks;
            }
            own = {mid, runs[victim].end};
            runs[victim].end = mid;
            ++steals;
        }
        return own.next++;
    };
    std::vector<std::exception_ptr> errors(workers);
    std::vector<std::thread> pool;
    pool.reserve(workers);
    // --debug-idle: per-thread finish timestamp, to check the runs and steals
    // keep every thread busy. See docs/RESEARCH.md (link above).
    const bool debug_idle = cfg.debug_idle;
    auto t0 = std::chrono::steady_clock::now();
    std::vector<double> finish(debug_idle ? workers : 0);
    for (unsigned w = 0; w < workers; ++w) {
        pool.emplace_back([&fn, &errors, &take, num_chunks, w, debug_idle, &finish, t0]() {
            try {
                for (unsigned idx = take(w, nullptr); idx < num_chunks; idx = take(w, &t_chunk_stats))
                    fn(idx);
                if (debug_idle) {
                    finish[w] = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                }
            } catch (...) {
                errors[w] = std::current_exception();
            }
        });
    }
    for (auto& th : pool) th.join();
    for (auto& e : errors) if (e) std::rethrow_exception(e);
    if (debug_idle && !finish.empty()) {
        double lo = finish[0], hi = finish[0], idle_sum = 0;
        for (double f : finish) { lo = std::min(lo, f); hi = std::max(hi, f); }
        for (double f : finish) idle_sum += (hi - f);
        double r_lo = 0, r_hi = 0;
        for (unsigned v = 0; v < workers; ++v) {
            const double r = rate(v) / 1e6;
            if (r > 0 && (r_lo == 0 || r < r_lo)) r_lo = r;
            r_hi = std::max(r_hi, r);
        }
        std::fprintf(stderr, "[idle] chunks=%u workers=%u steals=%u activation=%.1f ns/prime "
                     "rate=%.0f-%.0f Mk/s min=%.3fs max=%.3fs idle=%.1f%%\n",
                     num_chunks, workers, steals, activated ? 1e9 * activate_s / static_cast<double>(activated) : 0.0,
                     r_lo, r_hi, lo, hi, 100.0 * idle_sum / (workers * hi));
    }
}

// Everything main() has decided by the time the sieve starts, for the three
// output modes below.
struct RunPlan {
    const Options& opt;
    const Colors& C;
    std::vector<ChunkRange> ranges;
    unsigned actual_threads;
    const TierSet& wide;
    const TierSet& narrow;
    uint64_t narrow_k_end; // chunks with high <= this use `narrow`
    uint64_t base_limit;
    const Presieve& presieve;
    SieveConfig cfg;
    std::vector<uint64_t> fresh_primes;
    uint64_t total_span;
    uint64_t range_start;
    std::chrono::steady_clock::time_point t_start;

    const TierSet& tiers_for(const ChunkRange& r) const { return r.high <= narrow_k_end ? narrow : wide; }
};

// Count-only (no -o): a single pass, no I/O of any kind.
static int run_count(const RunPlan& plan) {
    const Colors& C = plan.C;
    const unsigned num_chunks = static_cast<unsigned>(plan.ranges.size());
    // NullSink skips even the to_chars conversion: sieve_and_emit only
    // keeps the running count.
    std::vector<uint64_t> prime_counts(num_chunks, 0);
    {
        std::atomic<uint64_t> progress{0};
        std::atomic<bool> done{false};
        std::thread prog(print_progress, std::cref(C), "counting", std::ref(progress), plan.total_span, std::ref(done));
        ProgressGuard guard{done, prog};

        run_parallel_chunks(plan.actual_threads, plan.ranges, plan.fresh_primes, plan.cfg, [&](unsigned i) {
            const ChunkRange& r = plan.ranges[i];
            count_only_worker(r, plan.tiers_for(r), plan.base_limit, plan.presieve, plan.cfg, prime_counts[i], progress);
        });
    } // guard destructs here: progress thread joined before the summary prints below

    // With --start, only the wheel primes inside [range_start, N] count
    // (none, for any realistic start), so the tail total matches e.g.
    // `primesieve START N -c` exactly.
    uint64_t total_primes = 0;
    for (uint64_t p : WHEEL_PRIMES) if (p >= plan.range_start) ++total_primes;
    for (auto c : prime_counts) total_primes += c;

    auto t_end = std::chrono::steady_clock::now();
    double total_s = std::chrono::duration<double>(t_end - plan.t_start).count();
    double total_mprimes = total_s > 0 ? (total_primes / 1e6 / total_s) : 0.0;

    std::string range_desc = plan.range_start
        ? "in [" + format_thousands(plan.range_start) + ", " + format_thousands(plan.opt.limit) + "]"
        : "up to " + format_thousands(plan.opt.limit);
    std::fprintf(stderr,
        "%sDone.%s %s%s%s primes found %s.\n"
        "  %stotal:%s      %s%.3fs%s (%s%.1f M primes/s%s)\n",
        C.headline, C.reset,
        C.bold, format_thousands(total_primes).c_str(), C.reset,
        range_desc.c_str(),
        C.headline, C.reset, C.time, total_s, C.reset, C.headline, total_mprimes, C.reset);

    return 0;
}

// -o *.db: a single pass, blocks to the .blk and index rows to SQLite.
static int run_db(const RunPlan& plan) {
    const Options& opt = plan.opt;
    const Colors& C = plan.C;
    const unsigned num_chunks = static_cast<unsigned>(plan.ranges.size());
    // Sieve + gap-encode + zstd, streamed to one dedicated writer thread
    // draining into SQLite (SqlitePrimeStore). No counting pre-pass: each
    // chunk's real prime count (prime_counts[i]) falls out of this same pass
    // (emit_db_worker tracks it via sieve_chunk's local_count); text output
    // still needs its own pre-pass because pwrite() wants exact byte offsets
    // up front, but .db blocks carry their own (chunk-relative) start_index
    // and go through an async queue -- see gap_block_sink.hpp and
    // SqlitePrimeStore::finish/fix_offsets for how start_index gets its true
    // global value afterwards, from these same counts.
    std::vector<uint64_t> prime_counts(num_chunks, 0);
    SqlitePrimeStore store(opt.output);
    {
        std::atomic<uint64_t> progress{0};
        std::atomic<bool> done{false};
        std::thread prog(print_progress, std::cref(C), "writing", std::ref(progress), plan.total_span, std::ref(done));
        ProgressGuard guard{done, prog};

        run_parallel_chunks(plan.actual_threads, plan.ranges, plan.fresh_primes, plan.cfg, [&](unsigned i) {
            const ChunkRange& r = plan.ranges[i];
            emit_db_worker(static_cast<int>(i), r, plan.tiers_for(r), plan.base_limit, plan.presieve, plan.cfg,
                           store, opt.db_block_size, opt.zstd_level, prime_counts[i], progress);
        });
    }

    prime_counts[0] += SMALL_PRIMES_COUNT;

    std::vector<uint64_t> chunk_offset(num_chunks, 0);
    for (unsigned i = 1; i < num_chunks; ++i) chunk_offset[i] = chunk_offset[i - 1] + prime_counts[i - 1];
    uint64_t total_primes = num_chunks ? chunk_offset.back() + prime_counts.back() : 0;

    store.finish(total_primes, opt.limit, WHEEL_MOD, opt.db_block_size, opt.zstd_level, chunk_offset);

    auto t_end = std::chrono::steady_clock::now();
    double total_s = std::chrono::duration<double>(t_end - plan.t_start).count();
    double total_mprimes = total_s > 0 ? (total_primes / 1e6 / total_s) : 0.0;
    // The index (.db) plus the blocks (.blk): what the output takes on disk.
    uint64_t db_bytes = fs::file_size(opt.output) + fs::file_size(blk_path_for(opt.output));
    double bytes_per_prime = total_primes ? static_cast<double>(db_bytes) / total_primes : 0.0;

    std::fprintf(stderr,
        "%sDone.%s %s%s%s primes found up to %s %s(%.2f GB, %.3f B/prime)%s.\n"
        "  %stotal:%s      %s%7.3fs%s  (%s%.1f M primes/s%s)\n",
        C.headline, C.reset,
        C.bold, format_thousands(total_primes).c_str(), C.reset,
        format_thousands(opt.limit).c_str(),
        C.dim, db_bytes / 1e9, bytes_per_prime, C.reset,
        C.headline, C.reset, C.time, total_s, C.reset, C.headline, total_mprimes, C.reset);

    return 0;
}

// -o text: two passes, byte counting then parallel pwrite (see the top-of-file comment).
static int run_text(const RunPlan& plan) {
    const Options& opt = plan.opt;
    const Colors& C = plan.C;
    const unsigned num_chunks = static_cast<unsigned>(plan.ranges.size());
    // --- Pass 1: byte counting (no I/O) ---
    std::vector<uint64_t> byte_counts(num_chunks, 0);
    std::vector<uint64_t> prime_counts(num_chunks, 0);
    {
        std::atomic<uint64_t> progress{0};
        std::atomic<bool> done{false};
        std::thread prog(print_progress, std::cref(C), "counting", std::ref(progress), plan.total_span, std::ref(done));
        ProgressGuard guard{done, prog};

        run_parallel_chunks(plan.actual_threads, plan.ranges, plan.fresh_primes, plan.cfg, [&](unsigned i) {
            const ChunkRange& r = plan.ranges[i];
            count_worker(r, plan.tiers_for(r), plan.base_limit, plan.presieve, plan.cfg, byte_counts[i], prime_counts[i], progress);
        });
    }

    byte_counts[0] += SMALL_PRIMES_BYTES;
    prime_counts[0] += SMALL_PRIMES_COUNT;

    std::vector<uint64_t> offsets(num_chunks, 0);
    for (unsigned i = 1; i < num_chunks; ++i) offsets[i] = offsets[i - 1] + byte_counts[i - 1];
    uint64_t total_bytes = offsets.back() + byte_counts.back();
    uint64_t total_primes = 0;
    for (auto c : prime_counts) total_primes += c;

    auto t_count_done = std::chrono::steady_clock::now();

    // --- Size the final file exactly ---
    {
        std::ofstream(opt.output, std::ios::binary | std::ios::trunc); // create/truncate
    }
    fs::resize_file(opt.output, total_bytes);

    int fd = ::open(opt.output.c_str(), O_WRONLY);
    if (fd < 0) {
        std::fprintf(stderr, "Error: could not open %s for writing\n", opt.output.c_str());
        return 1;
    }

    // --- Pass 2: parallel direct write ---
    {
        std::atomic<uint64_t> progress{0};
        std::atomic<bool> done{false};
        std::thread prog(print_progress, std::cref(C), "writing", std::ref(progress), plan.total_span, std::ref(done));
        ProgressGuard guard{done, prog};

        try {
            run_parallel_chunks(plan.actual_threads, plan.ranges, plan.fresh_primes, plan.cfg, [&](unsigned i) {
                const ChunkRange& r = plan.ranges[i];
                emit_worker(static_cast<int>(i), r, plan.tiers_for(r), plan.base_limit, plan.presieve, plan.cfg,
                            fd, offsets[i], progress);
            });
        } catch (...) {
            ::close(fd);
            throw;
        }
    }
    ::close(fd);

    auto t_end = std::chrono::steady_clock::now();
    double count_s = std::chrono::duration<double>(t_count_done - plan.t_start).count();
    double write_s = std::chrono::duration<double>(t_end - t_count_done).count();
    double total_s = std::chrono::duration<double>(t_end - plan.t_start).count();
    double count_mprimes = count_s > 0 ? (total_primes / 1e6 / count_s) : 0.0;
    double write_gbps = write_s > 0 ? (total_bytes / 1e9 / write_s) : 0.0;
    double total_mprimes = total_s > 0 ? (total_primes / 1e6 / total_s) : 0.0;

    std::fprintf(stderr,
        "%sDone.%s %s%s%s primes found up to %s %s(%.2f GB)%s.\n"
        "  %scount:%s      %s%7.3fs%s  (%s%.1f M primes/s%s)\n"
        "  %swrite:%s      %s%7.3fs%s  (%s%.2f GB/s%s)\n"
        "  %stotal:%s      %s%7.3fs%s  (%s%.1f M primes/s%s)\n",
        C.headline, C.reset,
        C.bold, format_thousands(total_primes).c_str(), C.reset,
        format_thousands(opt.limit).c_str(),
        C.dim, total_bytes / 1e9, C.reset,
        C.label, C.reset, C.time, count_s, C.reset, C.rate, count_mprimes, C.reset,
        C.label, C.reset, C.time, write_s, C.reset, C.io, write_gbps, C.reset,
        C.headline, C.reset, C.time, total_s, C.reset, C.headline, total_mprimes, C.reset);

    return 0;
}

int main(int argc, char** argv) {
    Options opt;
    try {
        opt = parse_args(argc, argv);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Error: %s\n\n", e.what());
        print_usage(argv[0]);
        return 1;
    }
    if (opt.show_help) {
        print_usage(argv[0]);
        return 0;
    }
    // --start (see below) is a count-only benchmarking aid: a partial
    // .txt/.db would still get the wheel primes and, for .db,
    // chunk-relative positions that aren't global prime indices.
    if (opt.start && !opt.output.empty()) {
        std::fprintf(stderr, "Error: --start only works in count mode (without -o).\n");
        return 1;
    }

    const Colors C(stderr_supports_color());

    auto t_start = std::chrono::steady_clock::now();

    if (opt.limit < 2) {
        if (opt.output.empty()) {
            std::fprintf(stderr, "Done. 0 primes found up to %llu.\n",
                         static_cast<unsigned long long>(opt.limit));
        } else if (is_db_output(opt.output)) {
            write_tiny_db(opt.output, {}, opt.limit, opt.db_block_size, opt.zstd_level);
            std::fprintf(stderr, "N < 2: no primes. Empty .db file created at %s\n", opt.output.c_str());
        } else {
            std::ofstream(opt.output, std::ios::binary | std::ios::trunc);
            std::fprintf(stderr, "N < 2: no primes. Empty file created at %s\n", opt.output.c_str());
        }
        return 0;
    }
    if (opt.limit < FIRST_WHEEL_PRIME) {
        // The wheel's own primes fall outside its numbering; for limits
        // this small there is no wheel range to sieve at all, so this is
        // resolved directly, without the parallel machinery.
        std::vector<uint64_t> small;
        for (uint64_t p : WHEEL_PRIMES) if (opt.limit >= p) small.push_back(p);
        if (opt.output.empty()) {
            std::fprintf(stderr, "Done. %zu prime(s) found up to %llu.\n",
                         small.size(), static_cast<unsigned long long>(opt.limit));
        } else if (is_db_output(opt.output)) {
            write_tiny_db(opt.output, small, opt.limit, opt.db_block_size, opt.zstd_level);
            std::fprintf(stderr, "Done. %zu prime(s) written to %s\n", small.size(), opt.output.c_str());
        } else {
            std::ofstream ofs(opt.output, std::ios::binary | std::ios::trunc);
            for (uint64_t p : small) ofs << p << "\n";
            std::fprintf(stderr, "Done. %zu prime(s) written to %s\n", small.size(), opt.output.c_str());
        }
        return 0;
    }

    uint64_t base_limit = isqrt(opt.limit);
    std::fprintf(stderr, "Computing base primes up to %llu...\n",
                 static_cast<unsigned long long>(base_limit));
    const BasePrimes base = sieve_base_primes(base_limit, opt.threads);
    std::fprintf(stderr, "  %llu base primes found.\n", static_cast<unsigned long long>(base.count));

    SievePlan P = plan_sieve(opt, base, base_limit);

    // Split into many more, narrower chunks than threads (see
    // run_parallel_chunks for why: work per chunk isn't uniform across the
    // range) and hand them out from a shared queue instead of one static
    // chunk per thread.
    // Floored so each chunk spans at least MIN_SEGS_PER_CHUNK segments: at
    // small N, threads*150 chunks would be narrower than one segment, and
    // per-chunk setup (SegmentSieve, re-activating every base prime) would
    // dominate. No effect at large N, where chunks span thousands of
    // segments. See
    // docs/RESEARCH.md#chunk-width-floor-at-least-4-segments-per-chunk-kept-2026-09-27
    // (4 until 2026-10-02, when sieve_chunk started carrying the sieve
    // across a worker's consecutive chunks).
    constexpr unsigned CHUNKS_PER_THREAD = 150;
    constexpr uint64_t MIN_SEGS_PER_CHUNK = 1;
    uint64_t width_cap = wheel_count_upto(opt.limit) / (MIN_SEGS_PER_CHUNK * P.seg_k_width);
    // No floor of one chunk per thread either: a range under threads *
    // MIN_SEGS_PER_CHUNK segments runs on fewer threads (actual_threads
    // below), each with at least that much work, instead of every thread
    // paying its own setup (SegmentSieve, activating every base prime) for a
    // sliver of a segment -- primesieve also drops to fewer threads for small
    // ranges. Only small N and short --start tails are affected (1e9 already
    // has 31 chunks of 4 segments).
    unsigned target_chunks = static_cast<unsigned>(std::max<uint64_t>(1,
        std::min<uint64_t>(uint64_t{opt.threads} * CHUNKS_PER_THREAD, width_cap)));
    // --start N0 (benchmarking only): sieve just [N0, N] instead of [0, N].
    // Every base prime is still activated for that range, so a tail of a
    // large N costs exactly what the same segments cost in a full run --
    // e.g. the last 1% of 1e14 in about a minute instead of the whole run.
    // The printed count is then only for that tail, not pi(N).
    uint64_t range_start = opt.start; // below N, parse_args
    if (range_start) {
        std::fprintf(stderr, "WARNING: --start %llu -- only [%llu, %llu] is sieved; the count is NOT pi(N).\n",
                     static_cast<unsigned long long>(range_start), static_cast<unsigned long long>(range_start),
                     static_cast<unsigned long long>(opt.limit));
        // Same chunk width as the full run where that still leaves every
        // thread TAIL_CHUNKS_PER_THREAD chunks. Short tails get more, narrower
        // chunks instead (never under MIN_SEGS_PER_CHUNK segments): the full
        // run's width would leave ~1.5 chunks per thread in a 1% tail and
        // cores idle in the last round (~20%), which skews wall-clock and, on
        // HT cores, cycles:u too. Chunks are cheap now that a worker carries
        // its sieve through its run (sieve_chunk): only the steal granularity
        // depends on them, so 32/thread (8 measured 3.6% idle on a 1e15 1%
        // tail, i5-13500, 20 threads, back when each chunk re-activated every
        // base prime).
        constexpr uint64_t TAIL_CHUNKS_PER_THREAD = 32;
        uint64_t span_k = wheel_count_upto(opt.limit) - wheel_count_upto(range_start - 1);
        double frac = static_cast<double>(span_k) / static_cast<double>(wheel_count_upto(opt.limit));
        uint64_t scaled = static_cast<uint64_t>(target_chunks * frac + 0.5);
        uint64_t floor_chunks = std::min<uint64_t>(uint64_t{opt.threads} * TAIL_CHUNKS_PER_THREAD,
                                                   span_k / (MIN_SEGS_PER_CHUNK * P.seg_k_width));
        target_chunks = static_cast<unsigned>(std::max<uint64_t>({uint64_t{1}, scaled, floor_chunks}));
    }
    auto ranges = split_ranges(opt.limit, target_chunks, range_start, P.seg_k_width);
    if (ranges.empty()) {
        // Can't happen with start < limit (the index just below a multiple
        // of 64 is always residue 29, so the next wheel number is 2 away),
        // but the passes below index ranges.back(): say so instead of
        // faulting if that invariant ever breaks.
        std::fprintf(stderr, "Done. 0 primes found in [%llu, %llu].\n",
                     static_cast<unsigned long long>(range_start), static_cast<unsigned long long>(opt.limit));
        return 0;
    }
    unsigned num_chunks = static_cast<unsigned>(ranges.size());
    unsigned actual_threads = std::min<unsigned>(opt.threads, num_chunks);

    // How many base primes a fresh start at each chunk activates (p*p below
    // its first segment's end), for run_parallel_chunks to price a steal.
    std::vector<uint64_t> fresh_primes(num_chunks);
    for (unsigned i = 0; i < num_chunks; ++i) {
        const ChunkRange& r = ranges[i];
        const uint64_t root = isqrt(wheel_number(std::min(r.low + P.tiers_for(r).width, r.high)));
        fresh_primes[i] = base.count_upto(root);
    }

    P.finish_threads(actual_threads);

    uint64_t total_span = ranges.back().high - ranges.front().low;

    Presieve presieve = build_presieve(PRESIEVE_GROUPS);

    print_plan(P, opt, actual_threads, ranges);

    // Every pass runs worker threads that can throw (pwrite() on a full disk,
    // or the bucket-sieve sizing check) -- see run_parallel_chunks for why
    // that needs this try/catch rather than main()'s existing one around
    // parse_args.
    const RunPlan plan{opt, C, std::move(ranges), actual_threads, P.wide, P.narrow, P.narrow_k_end, base_limit, presieve,
                       P.cfg, std::move(fresh_primes), total_span, range_start, t_start};
    try {
        if (opt.output.empty()) return run_count(plan);
        if (is_db_output(opt.output)) return run_db(plan);
        return run_text(plan);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "\nError: %s\n", e.what());
        return 1;
    }
}
