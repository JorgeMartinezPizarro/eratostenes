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
//   3. Text output (-o *.txt, the default) needs two passes, because
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
// --count-only skips the write/emit pass (and the file) entirely: a single
// pass (like .db's) already yields the total, so nothing else needs to run.
//
// The wheel's own primes (WHEEL_PRIMES) are special-cased in thread 0 --
// they don't take part in the wheel numbering, so they're just emitted
// directly instead of being found by sieving.

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdint>
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
#include "wheel.hpp"

namespace fs = std::filesystem;

// Dispatches -o/--output on its extension: ".db" (case-insensitive) means
// the SQLite + zstd gap-encoded format (gap_block_sink.hpp,
// sqlite_prime_store.hpp); anything else keeps the original one-prime-per-
// line text format.
static bool is_db_output(const std::string& path) {
    fs::path p(path);
    std::string ext = p.extension().string();
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".db";
}

// Writes a (small, already-known) list of primes straight into a .db store,
// with no threading -- used by the tiny-N early-return paths below, where
// N is too small for the parallel wheel machinery to apply at all.
static void write_tiny_db(const std::string& path, const std::vector<uint64_t>& primes,
                           uint64_t limit, uint64_t block_size, int zstd_level) {
    SqlitePrimeStore store(path);
    {
        GapBlockSink sink(0, block_size, zstd_level,
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

// L1-sized slice the small dense tier is crossed off in (SegmentSieve);
// set once in main() from the detected L1d size -- see that assignment's
// own comment for how a hybrid P-core/E-core CPU is handled: one
// conservative machine-wide value (the smallest domain detected), not a
// per-thread one. An earlier version sized each thread individually for
// wherever it happened to be running (sched_getcpu() + a per-CPU table);
// measured SLOWER on the actual target hardware (i5-13500, 2026-09,
// ~28-30s vs ~26-27s at N=1e12) than this simpler "smallest domain, same
// for everyone" version, even though the per-thread version was the
// mathematically "fairer" one -- see git history (both the per-thread
// version and the arithmetic bug it briefly had) for the full story.
static uint64_t SUB_BLOCK_BYTES = 32 * 1024;

struct ChunkRange {
    uint64_t low;   // first wheel index of the chunk (inclusive)
    uint64_t high;  // upper bound in wheel index (exclusive)
};

// Splits the wheel indices into 'threads' chunks as evenly as possible.
// k=0 is the number 1 (not prime; SegmentSieve clears it itself);
// k_end_exclusive is wheel_count_upto(limit), the first index whose number
// exceeds limit.
static std::vector<ChunkRange> split_ranges(uint64_t limit, unsigned threads) {
    std::vector<ChunkRange> ranges;
    // Starts at k=0 (the number 1, cleared by SegmentSieve itself) rather
    // than k=1, and every chunk boundary is a multiple of 64: the
    // byte-addressed dense tiers (erat_small.hpp) need each segment to
    // start on a word boundary.
    uint64_t k_start = 0;
    uint64_t k_end = wheel_count_upto(limit);
    if (k_end <= k_start) return ranges;

    uint64_t total = k_end - k_start;
    uint64_t per_thread = (total + threads - 1) / threads;
    per_thread = (per_thread + 63) / 64 * 64;

    uint64_t cursor = k_start;
    uint64_t remaining = total;
    for (unsigned t = 0; t < threads && remaining > 0; ++t) {
        uint64_t take = std::min(per_thread, remaining);
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
template <typename Writer>
static void sieve_chunk(ChunkRange range, uint64_t seg_k_width, uint64_t base_prime_max,
                         const std::vector<uint64_t>& small_primes,
                         const std::vector<uint64_t>& medium_primes,
                         const std::vector<uint64_t>& sparse_primes,
                         const Presieve& presieve,
                         Writer& out, uint64_t& local_count,
                         std::atomic<uint64_t>& progress) {
    SegmentSieve sieve(seg_k_width, base_prime_max, presieve, SUB_BLOCK_BYTES, !sparse_primes.empty());
    sieve.begin_chunk();
    for (uint64_t k_low = range.low; k_low < range.high; k_low += seg_k_width) {
        uint64_t k_high = std::min(k_low + seg_k_width, range.high);
        sieve.sieve_and_emit(k_low, k_high, small_primes, medium_primes, sparse_primes, out, local_count);
        progress.fetch_add(k_high - k_low, std::memory_order_relaxed);
    }
}

// Count-only pass: no I/O, no byte accounting, just the prime count.
static void count_only_worker(ChunkRange range, uint64_t seg_k_width, uint64_t base_prime_max,
                               const std::vector<uint64_t>& small_primes,
                               const std::vector<uint64_t>& medium_primes,
                               const std::vector<uint64_t>& sparse_primes,
                               const Presieve& presieve,
                               uint64_t& out_count, std::atomic<uint64_t>& progress) {
    NullSink sink;
    uint64_t local_count = 0;
    sieve_chunk(range, seg_k_width, base_prime_max, small_primes, medium_primes, sparse_primes, presieve, sink, local_count, progress);
    out_count = local_count;
}

// Byte-counting pass: no disk I/O, just measures how many text bytes each
// thread's primes will take.
static void count_worker(ChunkRange range, uint64_t seg_k_width, uint64_t base_prime_max,
                          const std::vector<uint64_t>& small_primes,
                          const std::vector<uint64_t>& medium_primes,
                          const std::vector<uint64_t>& sparse_primes,
                          const Presieve& presieve,
                          uint64_t& out_bytes, uint64_t& out_count,
                          std::atomic<uint64_t>& progress) {
    ByteCounter counter;
    uint64_t local_count = 0;
    sieve_chunk(range, seg_k_width, base_prime_max, small_primes, medium_primes, sparse_primes, presieve, counter, local_count, progress);
    out_bytes = counter.total_bytes;
    out_count = local_count;
}

// Write pass: re-sieves the same chunk and writes with pwrite() directly
// into its (disjoint) region of the final file.
static void emit_worker(int idx, ChunkRange range, uint64_t seg_k_width, uint64_t base_prime_max,
                         const std::vector<uint64_t>& small_primes,
                         const std::vector<uint64_t>& medium_primes,
                         const std::vector<uint64_t>& sparse_primes,
                         const Presieve& presieve,
                         int fd, uint64_t base_offset,
                         std::atomic<uint64_t>& progress) {
    DirectWriter out(fd, base_offset);
    uint64_t local_count = 0;

    if (idx == 0) {
        out.write_raw(SMALL_PRIMES_TEXT.data(), SMALL_PRIMES_BYTES);
    }

    sieve_chunk(range, seg_k_width, base_prime_max, small_primes, medium_primes, sparse_primes, presieve, out, local_count, progress);
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
static void emit_db_worker(int idx, ChunkRange range, uint64_t seg_k_width, uint64_t base_prime_max,
                            const std::vector<uint64_t>& small_primes,
                            const std::vector<uint64_t>& medium_primes,
                            const std::vector<uint64_t>& sparse_primes,
                            const Presieve& presieve,
                            SqlitePrimeStore& store,
                            uint64_t block_size, int zstd_level,
                            uint64_t& out_count,
                            std::atomic<uint64_t>& progress) {
    GapBlockSink sink(static_cast<uint64_t>(idx), block_size, zstd_level,
                       [&store](PendingBlock b) { store.push(std::move(b)); });
    uint64_t local_count = 0;

    if (idx == 0) {
        for (uint64_t p : WHEEL_PRIMES) sink.write_uint64(p);
    }

    sieve_chunk(range, seg_k_width, base_prime_max, small_primes, medium_primes, sparse_primes, presieve, sink, local_count, progress);
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

// Spawns 'workers' OS threads that dynamically pull chunk indices in
// [0, num_chunks) from a shared atomic counter and call fn(chunk_idx) for
// each, then joins them all and rethrows the first exception any of them
// raised. Plain std::thread has no way to propagate an exception back to
// the caller on its own -- one escaping a thread's function calls
// std::terminate() instead -- so every worker call is run under a
// try/catch that stashes it here (a full disk during the write pass, or
// the bucket-sieve sizing check in SegmentSieve::schedule, are the
// realistic ways this fires).
//
// num_chunks > workers on purpose (see split_ranges): a base prime only
// starts contributing hits to a segment once p*p is below that segment's
// position, so equal-WIDTH chunks are not equal-WORK chunks -- chunks near
// the end of the range have far more active base primes per segment than
// chunks near the start. Static one-chunk-per-thread assignment leaves
// early threads idle while the last one grinds through the most expensive
// part of the range; pulling many narrow chunks from a shared counter lets
// a thread that finishes an early, cheap chunk immediately pick up the
// next available one instead of sitting idle.
template <typename Fn>
static void run_parallel_chunks(unsigned workers, unsigned num_chunks, Fn&& fn) {
    std::atomic<unsigned> next_chunk{0};
    std::vector<std::exception_ptr> errors(workers);
    std::vector<std::thread> pool;
    pool.reserve(workers);
    for (unsigned w = 0; w < workers; ++w) {
        pool.emplace_back([&fn, &errors, &next_chunk, num_chunks, w]() {
            try {
                for (;;) {
                    unsigned idx = next_chunk.fetch_add(1, std::memory_order_relaxed);
                    if (idx >= num_chunks) break;
                    fn(idx);
                }
            } catch (...) {
                errors[w] = std::current_exception();
            }
        });
    }
    for (auto& th : pool) th.join();
    for (auto& e : errors) if (e) std::rethrow_exception(e);
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

    const Colors C(stderr_supports_color());

    auto t_start = std::chrono::steady_clock::now();

    if (opt.limit < 2) {
        if (opt.count_only) {
            std::fprintf(stderr, "Listo. 0 primos encontrados hasta %llu.\n",
                         static_cast<unsigned long long>(opt.limit));
        } else if (is_db_output(opt.output)) {
            write_tiny_db(opt.output, {}, opt.limit, opt.db_block_size, opt.zstd_level);
            std::fprintf(stderr, "N < 2: no hay primos. Fichero .db vacio creado en %s\n", opt.output.c_str());
        } else {
            std::ofstream(opt.output, std::ios::binary | std::ios::trunc);
            std::fprintf(stderr, "N < 2: no hay primos. Fichero vacio creado en %s\n", opt.output.c_str());
        }
        return 0;
    }
    if (opt.limit < FIRST_WHEEL_PRIME) {
        // The wheel's own primes fall outside its numbering; for limits
        // this small there is no wheel range to sieve at all, so this is
        // resolved directly, without the parallel machinery.
        std::vector<uint64_t> small;
        for (uint64_t p : WHEEL_PRIMES) if (opt.limit >= p) small.push_back(p);
        if (opt.count_only) {
            std::fprintf(stderr, "Listo. %zu primo(s) encontrado(s) hasta %llu.\n",
                         small.size(), static_cast<unsigned long long>(opt.limit));
        } else if (is_db_output(opt.output)) {
            write_tiny_db(opt.output, small, opt.limit, opt.db_block_size, opt.zstd_level);
            std::fprintf(stderr, "Listo. %zu primo(s) escrito(s) en %s\n", small.size(), opt.output.c_str());
        } else {
            std::ofstream ofs(opt.output, std::ios::binary | std::ios::trunc);
            for (uint64_t p : small) ofs << p << "\n";
            std::fprintf(stderr, "Listo. %zu primo(s) escrito(s) en %s\n", small.size(), opt.output.c_str());
        }
        return 0;
    }

    uint64_t base_limit = isqrt(opt.limit);
    std::fprintf(stderr, "Calculando primos base hasta %llu...\n",
                 static_cast<unsigned long long>(base_limit));
    std::vector<uint64_t> base_primes = sieve_base_primes(base_limit);
    std::fprintf(stderr, "  %zu primos base encontrados.\n", base_primes.size());

    // --segment-width is a numeric width (so the option keeps meaning the
    // same thing to the user); it's converted to a width in wheel indices
    // (WHEEL_SIZE useful numbers out of every WHEEL_MOD), rounded down to
    // whole 64-bit words: the byte-addressed dense tiers (erat_small.hpp)
    // need every segment to start on a word boundary.
    uint64_t seg_k_width = std::max<uint64_t>(64, (opt.segment_width * WHEEL_SIZE / WHEEL_MOD) / 64 * 64);

    // The small tier is crossed off one L1-sized sub-block at a time (see
    // SegmentSieve::sieve_and_emit), so the sub-block is the machine's real
    // L1 data cache (detected, like L2 for -s), and a prime counts as small
    // when it has >= ~16 hits per sub-block (p < sub-block bytes / 2; each
    // p-byte cycle holds 8 hits). Measured on an i5-11400F (48KiB L1d),
    // cycles:u at N=1e12 vs the old table/onfly tiers: 2414G ->
    // 1730G (32KiB), 1693G (48KiB), 1798G (64KiB), 1935G (128KiB), 1931G
    // (512KiB, i.e. no sub-blocking); cutoff at /4 and /1 both lost to /2
    // at every size -- a lower cutoff leaves too many hits on the slower
    // medium loop, a higher one pays the unrolled loop's unpredictable
    // entry/exit on primes with too few hits to amortize it.
    //
    // Re-checked (2026-09-24) after the medium tier's mod-210 stepping
    // (erat_small.hpp::cross_off_medium) made medium ~14% cheaper per hit:
    // hypothesis was that a cheaper medium tier should pull small_limit
    // down (fewer primes classified small, more ceded to the now-cheaper
    // medium). Measured (i5-11400F, perf stat cycles:u, N=1e12, single run
    // at a time): /2 (current, 1.4753T) vs /3 (1.4842T, +0.6%,
    // instructions:u +4.3%) vs *2/3 i.e. K=1.5 (1.4920T, +1.1%, despite
    // instructions:u -2.2%) -- both directions lost. The per-hit gap
    // between small (~2 instructions) and medium (~8-9, even after the
    // mod-210 cut) is still ~4x, far bigger than medium's 14% improvement,
    // so the optimal cutoff didn't move. /2 confirmed still optimal.
    uint64_t l1_bytes = opt.l1_bytes_override ? opt.l1_bytes_override : detect_l1d_cache_bytes();
    if (l1_bytes == 0) l1_bytes = 32 * 1024;
    SUB_BLOCK_BYTES = std::max<uint64_t>(8, l1_bytes / 8 * 8);
    uint64_t small_limit = SUB_BLOCK_BYTES / 2;

    // On a hybrid P-core/E-core CPU, detect_l2_cache_bytes()/
    // detect_l1d_cache_bytes() above always read cpu0 -- if cpu0 happens
    // to be a (bigger-cache) P-core, every thread, including E-core ones,
    // gets sized for cache they don't actually have that much of. Fix:
    // detect every CPU's own fair L2 share (CpuCacheTopology) and, if any
    // of them is SMALLER than what cpu0 alone gave us, use that smallest
    // one instead -- for every thread, uniformly, not per-thread. This
    // guarantees every thread's segment fits comfortably in whichever
    // cache it actually lands on, no matter which one that is.
    //
    // An earlier version sized each thread individually for wherever it
    // happened to be running (sched_getcpu() + a per-CPU table) instead of
    // this single conservative value -- measured SLOWER on the actual
    // target hardware (i5-13500, 2026-09: ~28-30s vs ~26-27s at N=1e12)
    // even though it was the mathematically "fairer" per-thread value.
    // The uniform, smallest-wins version tracks a run where the buggy
    // first version of the per-thread code (which -- by an unrelated
    // arithmetic bug, since fixed -- ended up dividing every thread's
    // share by an EXTRA 2 on top of the fair-share division) measured
    // fastest of all (~25-26s): not because the bug's exact numbers were
    // special, but because a smaller, safely-under-budget segment
    // (skipped once for every thread, not per-thread-recomputed) seems to
    // matter more on real many-thread-contended hardware than hitting
    // each core's own "fair" cache share exactly -- see git history for
    // the full A/B trail (dev PC and server) behind this. Skipped when
    // the user already forced a value on purpose (-s, --l2-bytes,
    // --l1-bytes) or detection found nothing (non-Linux, sysfs
    // unavailable).
    if ((!opt.segment_width_set && !opt.l2_bytes_override) || !opt.l1_bytes_override) {
        CpuCacheTopology topo = detect_cpu_cache_topology();
        // Gate on GENUINE heterogeneity (some other CPU's share is smaller
        // than cpu0's own) rather than always recomputing from the
        // minimum: on a uniform machine every share is equal, so the
        // minimum trivially equals cpu0's, and re-deriving through
        // seg_k_width_from_l2_bytes -- whose own /2 margin is deliberately
        // extra-conservative, validated for real P/E-core contention, see
        // that function's comment -- would apply that SAME extra margin
        // machine-wide for no reason, even where it's only ever been
        // measured to help (i5-13500) and was NOT re-validated to help
        // (this project's own i5-11400F data on this margin question is
        // mixed -- see git history). Only touch anything when the
        // machine actually has more than one cache domain.
        if (!opt.segment_width_set && !opt.l2_bytes_override && !topo.l2_share.empty() && topo.l2_share[0]) {
            uint64_t min_l2_share = topo.l2_share[0];
            for (uint64_t s : topo.l2_share) if (s && s < min_l2_share) min_l2_share = s;
            if (min_l2_share < topo.l2_share[0]) {
                seg_k_width = seg_k_width_from_l2_bytes(min_l2_share);
                opt.segment_width = seg_k_width * WHEEL_MOD / WHEEL_SIZE; // keep the startup log's "segmento=" accurate
            }
        }
        if (!opt.l1_bytes_override && !topo.l1_raw.empty() && topo.l1_raw[0]) {
            uint64_t min_l1_raw = topo.l1_raw[0];
            for (uint64_t s : topo.l1_raw) if (s && s < min_l1_raw) min_l1_raw = s;
            if (min_l1_raw < topo.l1_raw[0]) {
                SUB_BLOCK_BYTES = sub_block_from_l1_bytes(min_l1_raw);
                small_limit = SUB_BLOCK_BYTES / 2;
            }
        }
    }

    // EXPERIMENT IN PROGRESS (isolated test of point 1 from an external
    // review, Opus 5.5, 2026-09-24, see segment_sieve.hpp's sparse-tier
    // header comment): that tier's EratBig-style rewrite needs the segment
    // width in BYTES to be a power of 2 for its bucket-slot math to be a
    // shift/mask instead of a division. base_limit >= seg_k_width is a
    // conservative check for "will any base prime actually end up sparse"
    // (base_limit is isqrt(limit), an upper bound on the largest base
    // prime) -- when it's false, no prime is classified sparse below and
    // the width is left exactly as auto-tuned, same as before this
    // experiment. small_limit/the small-vs-medium cutoff are untouched.
    if (base_limit >= seg_k_width) {
        uint64_t sb = seg_k_width / 8, p2 = 1;
        while (p2 * 2 <= sb) p2 *= 2;
        if (p2 != sb) {
            seg_k_width = std::max<uint64_t>(64, p2 * 8);
            opt.segment_width = seg_k_width * WHEEL_MOD / WHEEL_SIZE; // keep the startup log's "segmento=" accurate
        }
    }

    // Primes also covered by the pre-sieve pattern (see presieve.hpp) are
    // skipped here: they're never scheduled as active markers, their
    // multiples come pre-marked from the pattern buffer instead. They
    // still come out as output/count -- nothing marks the primes
    // themselves composite either way, so they survive extraction exactly
    // as before.
    //
    // The rest split into three tiers by expected hits (see
    // segment_sieve.hpp / erat_small.hpp):
    //   - small_primes (p < small_limit): many hits per L1 sub-block,
    //     crossed off one sub-block at a time so the marks land in L1.
    //   - medium_primes (small_limit <= p < seg_k_width): a few hits per
    //     segment, one pass over the whole segment each.
    //   - sparse_primes (p >= seg_k_width): at most ~1 hit/segment, bucket
    //     ring, EratBig-style (see segment_sieve.hpp's process_big).
    std::vector<uint64_t> presieve_primes_flat;
    for (const auto& group : PRESIEVE_GROUPS)
        presieve_primes_flat.insert(presieve_primes_flat.end(), group.begin(), group.end());

    std::vector<uint64_t> small_primes;
    std::vector<uint64_t> medium_primes;
    std::vector<uint64_t> sparse_primes;
    for (uint64_t p : base_primes) {
        if (p < FIRST_WHEEL_PRIME) continue;
        if (std::find(presieve_primes_flat.begin(), presieve_primes_flat.end(), p) != presieve_primes_flat.end()) continue;
        if (p < seg_k_width) {
            (p < small_limit ? small_primes : medium_primes).push_back(p);
        } else {
            sparse_primes.push_back(p);
        }
    }

    // Split into many more, narrower chunks than threads (see
    // run_parallel_chunks for why: work per chunk isn't uniform across the
    // range) and hand them out from a shared queue instead of one static
    // chunk per thread.
    constexpr unsigned CHUNKS_PER_THREAD = 16;
    auto ranges = split_ranges(opt.limit, opt.threads * CHUNKS_PER_THREAD);
    unsigned num_chunks = static_cast<unsigned>(ranges.size());
    unsigned actual_threads = std::min<unsigned>(opt.threads, num_chunks);

    uint64_t total_span = ranges.back().high - ranges.front().low;

    Presieve presieve = build_presieve(PRESIEVE_GROUPS, seg_k_width);

    std::fprintf(stderr, "Iniciando %u hilos, limite=%llu, segmento=%llu, rueda mod %llu (%zu primos), "
                 "%zu primos base pequenos (sub-bloque %llu KiB), %zu medianos, %zu dispersos...\n",
                 actual_threads,
                 static_cast<unsigned long long>(opt.limit),
                 static_cast<unsigned long long>(opt.segment_width),
                 static_cast<unsigned long long>(WHEEL_MOD),
                 WHEEL_PRIMES.size(),
                 small_primes.size(), static_cast<unsigned long long>(SUB_BLOCK_BYTES / 1024),
                 medium_primes.size(), sparse_primes.size());

    // Every pass below runs worker threads that can throw (pwrite() on a
    // full disk, or the bucket-sieve sizing check) -- see run_parallel_chunks
    // for why that needs this try/catch rather than main()'s existing one
    // around parse_args.
    try {
        if (opt.count_only) {
            // Single pass, no I/O of any kind: NullSink skips even the
            // to_chars conversion, since we only need the running count that
            // sieve_and_emit already tracks internally.
            std::vector<uint64_t> prime_counts(num_chunks, 0);
            {
                std::atomic<uint64_t> progress{0};
                std::atomic<bool> done{false};
                std::thread prog(print_progress, std::cref(C), "contando", std::ref(progress), total_span, std::ref(done));
                ProgressGuard guard{done, prog};

                run_parallel_chunks(actual_threads, num_chunks, [&](unsigned i) {
                    count_only_worker(ranges[i], seg_k_width, base_limit, small_primes,
                                       medium_primes, sparse_primes, presieve, prime_counts[i], progress);
                });
            } // guard destructs here: progress thread joined before the summary prints below

            uint64_t total_primes = SMALL_PRIMES_COUNT;
            for (auto c : prime_counts) total_primes += c;

            auto t_end = std::chrono::steady_clock::now();
            double total_s = std::chrono::duration<double>(t_end - t_start).count();
            double total_mprimes = total_s > 0 ? (total_primes / 1e6 / total_s) : 0.0;

            std::fprintf(stderr,
                "%sListo.%s %s%s%s primos encontrados hasta %s.\n"
                "  %stotal:%s      %s%.2fs%s (%s%.1f M primos/s%s)\n",
                C.headline, C.reset,
                C.bold, format_thousands(total_primes).c_str(), C.reset,
                format_thousands(opt.limit).c_str(),
                C.headline, C.reset, C.time, total_s, C.reset, C.headline, total_mprimes, C.reset);

            return 0;
        }

        if (is_db_output(opt.output)) {
            // --- Single pass: sieve + gap-encode + zstd, streamed to one
            // dedicated writer thread draining into SQLite (see
            // SqlitePrimeStore). No separate counting pre-pass any more --
            // out_count[i] (each chunk's real prime count) falls out of
            // this same pass for free (emit_db_worker already tracks it
            // via sieve_chunk's local_count); text-output mode still needs
            // its own pre-pass because pwrite() requires exact byte offsets
            // up front, but .db blocks carry their own (chunk-relative)
            // start_index and go through an async queue, so nothing here
            // needs to be known before the sieve runs -- see
            // gap_block_sink.hpp and SqlitePrimeStore::finish/fix_offsets
            // for how start_index gets corrected to its true global value
            // afterwards, from these same counts.
            std::vector<uint64_t> prime_counts(num_chunks, 0);
            SqlitePrimeStore store(opt.output);
            {
                std::atomic<uint64_t> progress{0};
                std::atomic<bool> done{false};
                std::thread prog(print_progress, std::cref(C), "escribiendo", std::ref(progress), total_span, std::ref(done));
                ProgressGuard guard{done, prog};

                run_parallel_chunks(actual_threads, num_chunks, [&](unsigned i) {
                    emit_db_worker(static_cast<int>(i), ranges[i], seg_k_width, base_limit,
                                    small_primes, medium_primes, sparse_primes, presieve,
                                    store, opt.db_block_size, opt.zstd_level, prime_counts[i], progress);
                });
            }

            prime_counts[0] += SMALL_PRIMES_COUNT;

            std::vector<uint64_t> chunk_offset(num_chunks, 0);
            for (unsigned i = 1; i < num_chunks; ++i) chunk_offset[i] = chunk_offset[i - 1] + prime_counts[i - 1];
            uint64_t total_primes = num_chunks ? chunk_offset.back() + prime_counts.back() : 0;

            store.finish(total_primes, opt.limit, WHEEL_MOD, opt.db_block_size, opt.zstd_level, chunk_offset);

            auto t_end = std::chrono::steady_clock::now();
            double total_s = std::chrono::duration<double>(t_end - t_start).count();
            double total_mprimes = total_s > 0 ? (total_primes / 1e6 / total_s) : 0.0;
            uint64_t db_bytes = fs::file_size(opt.output);
            double bytes_per_prime = total_primes ? static_cast<double>(db_bytes) / total_primes : 0.0;

            std::fprintf(stderr,
                "%sListo.%s %s%s%s primos encontrados hasta %s %s(%.2f GB, %.3f B/primo)%s.\n"
                "  %stotal:%s      %s%6.2fs%s  (%s%.1f M primos/s%s)\n",
                C.headline, C.reset,
                C.bold, format_thousands(total_primes).c_str(), C.reset,
                format_thousands(opt.limit).c_str(),
                C.dim, db_bytes / 1e9, bytes_per_prime, C.reset,
                C.headline, C.reset, C.time, total_s, C.reset, C.headline, total_mprimes, C.reset);

            return 0;
        }

        // --- Pass 1: byte counting (no I/O) ---
        std::vector<uint64_t> byte_counts(num_chunks, 0);
        std::vector<uint64_t> prime_counts(num_chunks, 0);
        {
            std::atomic<uint64_t> progress{0};
            std::atomic<bool> done{false};
            std::thread prog(print_progress, std::cref(C), "contando", std::ref(progress), total_span, std::ref(done));
            ProgressGuard guard{done, prog};

            run_parallel_chunks(actual_threads, num_chunks, [&](unsigned i) {
                count_worker(ranges[i], seg_k_width, base_limit, small_primes,
                             medium_primes, sparse_primes, presieve, byte_counts[i], prime_counts[i], progress);
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
            std::fprintf(stderr, "Error: no se pudo abrir %s para escritura\n", opt.output.c_str());
            return 1;
        }

        // --- Pass 2: parallel direct write ---
        {
            std::atomic<uint64_t> progress{0};
            std::atomic<bool> done{false};
            std::thread prog(print_progress, std::cref(C), "escribiendo", std::ref(progress), total_span, std::ref(done));
            ProgressGuard guard{done, prog};

            try {
                run_parallel_chunks(actual_threads, num_chunks, [&](unsigned i) {
                    emit_worker(static_cast<int>(i), ranges[i], seg_k_width, base_limit,
                                small_primes, medium_primes, sparse_primes, presieve, fd, offsets[i], progress);
                });
            } catch (...) {
                ::close(fd);
                throw;
            }
        }
        ::close(fd);

        auto t_end = std::chrono::steady_clock::now();
        double count_s = std::chrono::duration<double>(t_count_done - t_start).count();
        double write_s = std::chrono::duration<double>(t_end - t_count_done).count();
        double total_s = std::chrono::duration<double>(t_end - t_start).count();
        double count_mprimes = count_s > 0 ? (total_primes / 1e6 / count_s) : 0.0;
        double write_gbps = write_s > 0 ? (total_bytes / 1e9 / write_s) : 0.0;
        double total_mprimes = total_s > 0 ? (total_primes / 1e6 / total_s) : 0.0;

        std::fprintf(stderr,
            "%sListo.%s %s%s%s primos encontrados hasta %s %s(%.2f GB)%s.\n"
            "  %sconteo:%s     %s%6.2fs%s  (%s%.1f M primos/s%s)\n"
            "  %sescritura:%s  %s%6.2fs%s  (%s%.2f GB/s%s)\n"
            "  %stotal:%s      %s%6.2fs%s  (%s%.1f M primos/s%s)\n",
            C.headline, C.reset,
            C.bold, format_thousands(total_primes).c_str(), C.reset,
            format_thousands(opt.limit).c_str(),
            C.dim, total_bytes / 1e9, C.reset,
            C.label, C.reset, C.time, count_s, C.reset, C.rate, count_mprimes, C.reset,
            C.label, C.reset, C.time, write_s, C.reset, C.io, write_gbps, C.reset,
            C.headline, C.reset, C.time, total_s, C.reset, C.headline, total_mprimes, C.reset);

        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "\nError: %s\n", e.what());
        return 1;
    }
}
