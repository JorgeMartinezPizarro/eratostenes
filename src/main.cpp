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
//   3. COUNT PASS: each thread sieves its chunk and counts how many bytes
//      of text its primes will take (writes nothing to disk). From those
//      totals, prefix sums give the exact offset where each thread must
//      start writing in the final file.
//   4. The final file is resized to its exact, already-known size.
//   5. WRITE PASS: each thread re-sieves its chunk (same work) and this
//      time writes with pwrite() directly into its (disjoint) region of
//      the final file, in parallel with every other thread.
//
// This design avoids the "write to temp files + merge" pattern, which
// doubles disk I/O (every output byte gets written twice). Here every byte
// of the final result is written exactly once; the extra cost is repeating
// the bit-marking phase (cheap, CPU/cache-bound) instead of repeating a
// disk-to-disk copy (expensive, I/O-bound).
//
// --count-only skips the write pass (and the file) entirely: the count
// pass alone already yields the total, so nothing else needs to run.
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
    store.finish(primes.size(), limit, WHEEL_MOD, block_size, zstd_level);
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

struct ChunkRange {
    uint64_t low;   // first wheel index of the chunk (inclusive)
    uint64_t high;  // upper bound in wheel index (exclusive)
};

// Splits the wheel indices into 'threads' chunks as evenly as possible.
// k=1 is the first useful index (k=0 would be n=1, which isn't prime);
// k_end_exclusive is wheel_count_upto(limit), the first index whose number
// exceeds limit.
static std::vector<ChunkRange> split_ranges(uint64_t limit, unsigned threads) {
    std::vector<ChunkRange> ranges;
    uint64_t k_start = 1;
    uint64_t k_end = wheel_count_upto(limit);
    if (k_end <= k_start) return ranges;

    uint64_t total = k_end - k_start;
    uint64_t per_thread = (total + threads - 1) / threads;
    if (per_thread == 0) per_thread = 1;

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
                         const std::vector<WheelBasePrime>& wheel_base_primes,
                         const std::vector<OnFlyPrime>& dense_onfly_primes,
                         const std::vector<uint64_t>& sparse_primes,
                         const Presieve& presieve,
                         Writer& out, uint64_t& local_count,
                         std::atomic<uint64_t>& progress) {
    SegmentSieve sieve(seg_k_width, base_prime_max, presieve);
    sieve.begin_chunk();
    for (uint64_t k_low = range.low; k_low < range.high; k_low += seg_k_width) {
        uint64_t k_high = std::min(k_low + seg_k_width, range.high);
        sieve.sieve_and_emit(k_low, k_high, wheel_base_primes, dense_onfly_primes, sparse_primes, out, local_count);
        progress.fetch_add(k_high - k_low, std::memory_order_relaxed);
    }
}

// Count-only pass: no I/O, no byte accounting, just the prime count.
static void count_only_worker(ChunkRange range, uint64_t seg_k_width, uint64_t base_prime_max,
                               const std::vector<WheelBasePrime>& wheel_base_primes,
                               const std::vector<OnFlyPrime>& dense_onfly_primes,
                               const std::vector<uint64_t>& sparse_primes,
                               const Presieve& presieve,
                               uint64_t& out_count, std::atomic<uint64_t>& progress) {
    NullSink sink;
    uint64_t local_count = 0;
    sieve_chunk(range, seg_k_width, base_prime_max, wheel_base_primes, dense_onfly_primes, sparse_primes, presieve, sink, local_count, progress);
    out_count = local_count;
}

// Byte-counting pass: no disk I/O, just measures how many text bytes each
// thread's primes will take.
static void count_worker(ChunkRange range, uint64_t seg_k_width, uint64_t base_prime_max,
                          const std::vector<WheelBasePrime>& wheel_base_primes,
                          const std::vector<OnFlyPrime>& dense_onfly_primes,
                          const std::vector<uint64_t>& sparse_primes,
                          const Presieve& presieve,
                          uint64_t& out_bytes, uint64_t& out_count,
                          std::atomic<uint64_t>& progress) {
    ByteCounter counter;
    uint64_t local_count = 0;
    sieve_chunk(range, seg_k_width, base_prime_max, wheel_base_primes, dense_onfly_primes, sparse_primes, presieve, counter, local_count, progress);
    out_bytes = counter.total_bytes;
    out_count = local_count;
}

// Write pass: re-sieves the same chunk and writes with pwrite() directly
// into its (disjoint) region of the final file.
static void emit_worker(int idx, ChunkRange range, uint64_t seg_k_width, uint64_t base_prime_max,
                         const std::vector<WheelBasePrime>& wheel_base_primes,
                         const std::vector<OnFlyPrime>& dense_onfly_primes,
                         const std::vector<uint64_t>& sparse_primes,
                         const Presieve& presieve,
                         int fd, uint64_t base_offset,
                         std::atomic<uint64_t>& progress) {
    DirectWriter out(fd, base_offset);
    uint64_t local_count = 0;

    if (idx == 0) {
        out.write_raw(SMALL_PRIMES_TEXT.data(), SMALL_PRIMES_BYTES);
    }

    sieve_chunk(range, seg_k_width, base_prime_max, wheel_base_primes, dense_onfly_primes, sparse_primes, presieve, out, local_count, progress);
    out.flush();
}

// .db write pass: re-sieves the same chunk and feeds a GapBlockSink, which
// gap-encodes/compresses in BLOCK_SIZE-prime blocks and pushes each one to
// 'store' (thread-safe, see SqlitePrimeStore::push) as it completes -- no
// disjoint-region bookkeeping needed here, unlike DirectWriter/pwrite,
// since blocks carry their own start_index and SQLite doesn't care what
// order rows are inserted in.
static void emit_db_worker(int idx, ChunkRange range, uint64_t seg_k_width, uint64_t base_prime_max,
                            const std::vector<WheelBasePrime>& wheel_base_primes,
                            const std::vector<OnFlyPrime>& dense_onfly_primes,
                            const std::vector<uint64_t>& sparse_primes,
                            const Presieve& presieve,
                            SqlitePrimeStore& store, uint64_t start_index,
                            uint64_t block_size, int zstd_level,
                            std::atomic<uint64_t>& progress) {
    GapBlockSink sink(start_index, block_size, zstd_level,
                       [&store](PendingBlock b) { store.push(std::move(b)); });
    uint64_t local_count = 0;

    if (idx == 0) {
        for (uint64_t p : WHEEL_PRIMES) sink.write_uint64(p);
    }

    sieve_chunk(range, seg_k_width, base_prime_max, wheel_base_primes, dense_onfly_primes, sparse_primes, presieve, sink, local_count, progress);
    sink.flush();
}

static void print_progress(const Colors& C, const char* label, std::atomic<uint64_t>& progress,
                            uint64_t total, std::atomic<bool>& done) {
    using namespace std::chrono_literals;
    while (!done.load()) {
        std::this_thread::sleep_for(500ms);
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
    ~ProgressGuard() { done = true; th.join(); }
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
    // (WHEEL_SIZE useful numbers out of every WHEEL_MOD).
    uint64_t seg_k_width = std::max<uint64_t>(64, opt.segment_width * WHEEL_SIZE / WHEEL_MOD);

    // Primes also covered by the pre-sieve pattern (see presieve.hpp) are
    // skipped here: they're never scheduled as active markers, their
    // multiples come pre-marked from the pattern buffer instead. They
    // still come out as output/count -- nothing marks the primes
    // themselves composite either way, so they survive extraction exactly
    // as before.
    //
    // The rest split into three tiers by expected hits per segment (see
    // segment_sieve.hpp and wheel.hpp/wheel_delta_at for the reasoning):
    //   - wheel_base_primes (p < seg_k_width, up to TABLE_PRIME_BUDGET of
    //     them): keeps a per-phase delta[] table. Reusing a cache-resident
    //     table entry beats recomputing on every hit, but only as long as
    //     the *whole* table stays cache-resident -- so this tier is capped
    //     by total table BYTES, not by a prime value or a hit-count
    //     estimate: base_primes is already sorted ascending, so taking the
    //     first TABLE_PRIME_BUDGET dense primes (by count) is exactly "the
    //     table never exceeds TABLE_BYTES_BUDGET," which is what actually
    //     determines whether a table lookup or recomputing wins -- and
    //     which primes end up in this tier at any given N falls out of
    //     that count on its own, no threshold on p needed.
    //   - dense_onfly_primes (remaining p < seg_k_width): no per-prime
    //     table -- each phase's advance comes from a tiny SHARED table
    //     instead (ONFLY_CORRECTION, wheel.hpp), so this tier's per-hit
    //     cost stays cheap (one multiply, one lookup, one add) no matter
    //     how many primes end up here; this is the tier whose per-prime
    //     table, summed across all its primes, used to stop fitting L3
    //     before that table was dropped in favor of the shared one.
    //   - sparse_primes (p >= seg_k_width): at most ~1 hit/segment, plain
    //     uint64_t, recovers an absolute multiplier from k (unchanged --
    //     see segment_sieve.hpp for why an ONFLY_CORRECTION-style shared
    //     table was tried here too and reverted, measured slower).
    //     Once the auto segment width gets L2-capped below sqrt(N), a
    //     growing fraction of base primes land here instead of
    //     dense_onfly_primes above -- the real driver of the 1e13 cliff
    //     (README#benchmarks), still open.
    //
    // TABLE_BYTES_BUDGET scales with the machine's real L3 (detected, not
    // guessed -- see detect_l3_cache_bytes in arg_parser.hpp): a fixed
    // small budget (this used to be a flat 2MiB) looked fine up to ~1e12,
    // where the *count* of dense primes past the budget was still small
    // enough that the extra per-hit cost of onfly/sparse barely showed --
    // but at 1e13+, pi(sqrt(N)) grows well past what 2MiB covers regardless
    // of machine, so a fixed budget makes every machine behave like the
    // smallest-L3 one: it's not that a bigger budget never helps, it's
    // that N wasn't large enough yet to need one. Half of L3 leaves room
    // for onfly/sparse's own (much smaller) per-prime arrays, presieve,
    // and whatever else shares L3 (other threads' segment buffers, if
    // -s wasn't capped small enough to stay in L2 -- see the auto -s
    // default). Falls back to a conservative 4MiB if L3 can't be detected.
    uint64_t l3_bytes = detect_l3_cache_bytes();
    if (l3_bytes == 0) l3_bytes = 4 * 1024 * 1024;
    size_t TABLE_BYTES_BUDGET = static_cast<size_t>(l3_bytes / 2);
    size_t TABLE_PRIME_BUDGET = TABLE_BYTES_BUDGET / sizeof(WheelBasePrime);
    std::vector<uint64_t> presieve_primes_flat;
    for (const auto& group : PRESIEVE_GROUPS)
        presieve_primes_flat.insert(presieve_primes_flat.end(), group.begin(), group.end());

    std::vector<WheelBasePrime> wheel_base_primes;
    std::vector<OnFlyPrime> dense_onfly_primes;
    std::vector<uint64_t> sparse_primes;
    wheel_base_primes.reserve(std::min(base_primes.size(), TABLE_PRIME_BUDGET));
    for (uint64_t p : base_primes) {
        if (p < FIRST_WHEEL_PRIME) continue;
        if (std::find(presieve_primes_flat.begin(), presieve_primes_flat.end(), p) != presieve_primes_flat.end()) continue;
        if (p < seg_k_width) {
            if (wheel_base_primes.size() < TABLE_PRIME_BUDGET) {
                wheel_base_primes.push_back({p, compute_wheel_deltas(p)});
            } else {
                dense_onfly_primes.push_back({p, p / WHEEL_MOD, static_cast<uint32_t>(WHEEL_POS[p % WHEEL_MOD])});
            }
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
                 "%zu primos base densos (tabla), %zu densos (recalculo), %zu dispersos...\n",
                 actual_threads,
                 static_cast<unsigned long long>(opt.limit),
                 static_cast<unsigned long long>(opt.segment_width),
                 static_cast<unsigned long long>(WHEEL_MOD),
                 WHEEL_PRIMES.size(),
                 wheel_base_primes.size(), dense_onfly_primes.size(), sparse_primes.size());

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
                    count_only_worker(ranges[i], seg_k_width, base_limit, wheel_base_primes,
                                       dense_onfly_primes, sparse_primes, presieve, prime_counts[i], progress);
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
            // --- Pass 1: prime counting (NullSink -- cheaper than the text
            // path's ByteCounter, since block sizing only needs how many
            // primes each thread finds, not their text width) ---
            std::vector<uint64_t> prime_counts(num_chunks, 0);
            {
                std::atomic<uint64_t> progress{0};
                std::atomic<bool> done{false};
                std::thread prog(print_progress, std::cref(C), "contando", std::ref(progress), total_span, std::ref(done));
                ProgressGuard guard{done, prog};

                run_parallel_chunks(actual_threads, num_chunks, [&](unsigned i) {
                    count_only_worker(ranges[i], seg_k_width, base_limit, wheel_base_primes,
                                       dense_onfly_primes, sparse_primes, presieve, prime_counts[i], progress);
                });
            }

            prime_counts[0] += SMALL_PRIMES_COUNT;

            std::vector<uint64_t> prime_offset(num_chunks, 0);
            for (unsigned i = 1; i < num_chunks; ++i) prime_offset[i] = prime_offset[i - 1] + prime_counts[i - 1];
            uint64_t total_primes = prime_offset.back() + prime_counts.back();

            auto t_count_done = std::chrono::steady_clock::now();

            // --- Pass 2: parallel sieve + gap-encode + zstd, one dedicated
            // writer thread draining into SQLite (see SqlitePrimeStore) ---
            SqlitePrimeStore store(opt.output);
            {
                std::atomic<uint64_t> progress{0};
                std::atomic<bool> done{false};
                std::thread prog(print_progress, std::cref(C), "escribiendo", std::ref(progress), total_span, std::ref(done));
                ProgressGuard guard{done, prog};

                run_parallel_chunks(actual_threads, num_chunks, [&](unsigned i) {
                    emit_db_worker(static_cast<int>(i), ranges[i], seg_k_width, base_limit,
                                    wheel_base_primes, dense_onfly_primes, sparse_primes, presieve,
                                    store, prime_offset[i], opt.db_block_size, opt.zstd_level, progress);
                });
            }
            store.finish(total_primes, opt.limit, WHEEL_MOD, opt.db_block_size, opt.zstd_level);

            auto t_end = std::chrono::steady_clock::now();
            double count_s = std::chrono::duration<double>(t_count_done - t_start).count();
            double write_s = std::chrono::duration<double>(t_end - t_count_done).count();
            double total_s = std::chrono::duration<double>(t_end - t_start).count();
            double count_mprimes = count_s > 0 ? (total_primes / 1e6 / count_s) : 0.0;
            double total_mprimes = total_s > 0 ? (total_primes / 1e6 / total_s) : 0.0;
            uint64_t db_bytes = fs::file_size(opt.output);
            double bytes_per_prime = total_primes ? static_cast<double>(db_bytes) / total_primes : 0.0;

            std::fprintf(stderr,
                "%sListo.%s %s%s%s primos encontrados hasta %s %s(%.2f GB, %.3f B/primo)%s.\n"
                "  %sconteo:%s     %s%6.2fs%s  (%s%.1f M primos/s%s)\n"
                "  %sescritura:%s  %s%6.2fs%s\n"
                "  %stotal:%s      %s%6.2fs%s  (%s%.1f M primos/s%s)\n",
                C.headline, C.reset,
                C.bold, format_thousands(total_primes).c_str(), C.reset,
                format_thousands(opt.limit).c_str(),
                C.dim, db_bytes / 1e9, bytes_per_prime, C.reset,
                C.label, C.reset, C.time, count_s, C.reset, C.rate, count_mprimes, C.reset,
                C.label, C.reset, C.time, write_s, C.reset,
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
                count_worker(ranges[i], seg_k_width, base_limit, wheel_base_primes,
                             dense_onfly_primes, sparse_primes, presieve, byte_counts[i], prime_counts[i], progress);
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
                                wheel_base_primes, dense_onfly_primes, sparse_primes, presieve, fd, offsets[i], progress);
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
