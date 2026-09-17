// Segmented, parallel, bit-packed Sieve of Eratosthenes, on a mod-2310
// wheel (2*3*5*7*11 -- skips multiples of those five primes up front).
//
// Strategy:
//   1. Compute the base primes (<= sqrt(N)) with a simple sieve.
//   2. The "wheel index" range [1, wheel_count_upto(N)) -- which enumerates
//      the numbers coprime with 2310 in (11, N], see wheel.hpp -- is split
//      into T contiguous chunks, one per thread.
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
// 2, 3, 5, 7 and 11 are the only primes that are multiples of themselves
// among the wheel's own primes, and are special-cased in thread 0 (they
// don't take part in the wheel numbering).

#include <cstdio>
#include <cstdint>
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
#include "segment_sieve.hpp"
#include "sinks.hpp"
#include "wheel.hpp"

namespace fs = std::filesystem;

// The smallest prime not covered by the wheel (2,3,5,7,11) itself.
constexpr uint64_t FIRST_WHEEL_PRIME = 13;
constexpr const char* SMALL_PRIMES_TEXT = "2\n3\n5\n7\n11\n";
constexpr uint64_t SMALL_PRIMES_BYTES = 11; // strlen(SMALL_PRIMES_TEXT)
constexpr uint64_t SMALL_PRIMES_COUNT = 5;

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
static void sieve_chunk(ChunkRange range, uint64_t seg_k_width,
                         const std::vector<WheelBasePrime>& wheel_base_primes,
                         Writer& out, uint64_t& local_count,
                         std::atomic<uint64_t>& progress) {
    SegmentSieve sieve(seg_k_width);
    sieve.begin_chunk(range.low, wheel_base_primes);
    for (uint64_t k_low = range.low; k_low < range.high; k_low += seg_k_width) {
        uint64_t k_high = std::min(k_low + seg_k_width, range.high);
        sieve.sieve_and_emit(k_low, k_high, wheel_base_primes, out, local_count);
        progress.fetch_add(k_high - k_low, std::memory_order_relaxed);
    }
}

// Count-only pass: no I/O, no byte accounting, just the prime count.
static void count_only_worker(ChunkRange range, uint64_t seg_k_width,
                               const std::vector<WheelBasePrime>& wheel_base_primes,
                               uint64_t& out_count, std::atomic<uint64_t>& progress) {
    NullSink sink;
    uint64_t local_count = 0;
    sieve_chunk(range, seg_k_width, wheel_base_primes, sink, local_count, progress);
    out_count = local_count;
}

// Byte-counting pass: no disk I/O, just measures how many text bytes each
// thread's primes will take.
static void count_worker(ChunkRange range, uint64_t seg_k_width,
                          const std::vector<WheelBasePrime>& wheel_base_primes,
                          uint64_t& out_bytes, uint64_t& out_count,
                          std::atomic<uint64_t>& progress) {
    ByteCounter counter;
    uint64_t local_count = 0;
    sieve_chunk(range, seg_k_width, wheel_base_primes, counter, local_count, progress);
    out_bytes = counter.total_bytes;
    out_count = local_count;
}

// Write pass: re-sieves the same chunk and writes with pwrite() directly
// into its (disjoint) region of the final file.
static void emit_worker(int idx, ChunkRange range, uint64_t seg_k_width,
                         const std::vector<WheelBasePrime>& wheel_base_primes,
                         int fd, uint64_t base_offset,
                         std::atomic<uint64_t>& progress) {
    DirectWriter out(fd, base_offset);
    uint64_t local_count = 0;

    if (idx == 0) {
        out.write_raw(SMALL_PRIMES_TEXT, SMALL_PRIMES_BYTES);
    }

    sieve_chunk(range, seg_k_width, wheel_base_primes, out, local_count, progress);
    out.flush();
}

static void print_progress(const char* label, std::atomic<uint64_t>& progress,
                            uint64_t total, std::atomic<bool>& done) {
    using namespace std::chrono_literals;
    while (!done.load()) {
        std::this_thread::sleep_for(500ms);
        if (done.load()) break;
        uint64_t d = progress.load(std::memory_order_relaxed);
        double pct = total ? std::min(100.0, 100.0 * d / total) : 100.0;
        std::fprintf(stderr, "\r  %s: %5.1f%%   ", label, pct);
        std::fflush(stderr);
    }
    std::fprintf(stderr, "\r  %s: 100.0%%   \n", label);
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

    auto t_start = std::chrono::steady_clock::now();

    if (opt.limit < 2) {
        if (opt.count_only) {
            std::fprintf(stderr, "Listo. 0 primos encontrados hasta %llu.\n",
                         static_cast<unsigned long long>(opt.limit));
        } else {
            std::ofstream(opt.output, std::ios::binary | std::ios::trunc);
            std::fprintf(stderr, "N < 2: no hay primos. Fichero vacio creado en %s\n", opt.output.c_str());
        }
        return 0;
    }
    if (opt.limit < FIRST_WHEEL_PRIME) {
        // 2, 3, 5, 7 and 11 fall outside the mod-2310 wheel numbering; for
        // limits this small there is no wheel range to sieve at all, so
        // this is resolved directly, without the parallel machinery.
        unsigned n = 0;
        for (uint64_t p : {2ULL, 3ULL, 5ULL, 7ULL, 11ULL}) if (opt.limit >= p) ++n;
        if (opt.count_only) {
            std::fprintf(stderr, "Listo. %u primo(s) encontrado(s) hasta %llu.\n",
                         n, static_cast<unsigned long long>(opt.limit));
        } else {
            std::ofstream ofs(opt.output, std::ios::binary | std::ios::trunc);
            for (uint64_t p : {2ULL, 3ULL, 5ULL, 7ULL, 11ULL}) if (opt.limit >= p) ofs << p << "\n";
            std::fprintf(stderr, "Listo. %u primo(s) escrito(s) en %s\n", n, opt.output.c_str());
        }
        return 0;
    }

    uint64_t base_limit = isqrt(opt.limit);
    std::fprintf(stderr, "Calculando primos base hasta %llu...\n",
                 static_cast<unsigned long long>(base_limit));
    std::vector<uint64_t> base_primes = sieve_base_primes(base_limit);
    std::fprintf(stderr, "  %zu primos base encontrados.\n", base_primes.size());

    // Per-prime wheel jump table (2, 3, 5, 7 and 11 don't need one: they
    // are special-cased). Computed once here, not once per segment.
    std::vector<WheelBasePrime> wheel_base_primes;
    wheel_base_primes.reserve(base_primes.size());
    for (uint64_t p : base_primes) {
        if (p < FIRST_WHEEL_PRIME) continue;
        wheel_base_primes.push_back({p, compute_wheel_deltas(p)});
    }

    auto ranges = split_ranges(opt.limit, opt.threads);
    unsigned actual_threads = static_cast<unsigned>(ranges.size());

    uint64_t total_span = ranges.back().high - ranges.front().low;

    // --segment-width is a numeric width (so the option keeps meaning the
    // same thing to the user); it's converted to a width in wheel indices
    // (WHEEL_SIZE useful numbers out of every WHEEL_MOD).
    uint64_t seg_k_width = std::max<uint64_t>(64, opt.segment_width * WHEEL_SIZE / WHEEL_MOD);

    std::fprintf(stderr, "Iniciando %u hilos, limite=%llu, segmento=%llu...\n",
                 actual_threads,
                 static_cast<unsigned long long>(opt.limit),
                 static_cast<unsigned long long>(opt.segment_width));

    if (opt.count_only) {
        // Single pass, no I/O of any kind: NullSink skips even the
        // to_chars conversion, since we only need the running count that
        // sieve_and_emit already tracks internally.
        std::vector<uint64_t> prime_counts(actual_threads, 0);
        std::atomic<uint64_t> progress{0};
        std::atomic<bool> done{false};
        std::thread prog(print_progress, "contando", std::ref(progress), total_span, std::ref(done));

        std::vector<std::thread> pool;
        for (unsigned i = 0; i < actual_threads; ++i) {
            pool.emplace_back(count_only_worker, ranges[i], seg_k_width, std::cref(wheel_base_primes),
                               std::ref(prime_counts[i]), std::ref(progress));
        }
        for (auto& th : pool) th.join();
        done = true;
        prog.join();

        uint64_t total_primes = SMALL_PRIMES_COUNT;
        for (auto c : prime_counts) total_primes += c;

        auto t_end = std::chrono::steady_clock::now();
        double total_s = std::chrono::duration<double>(t_end - t_start).count();

        std::fprintf(stderr,
            "Listo. %llu primos encontrados hasta %llu.\n"
            "  total:      %.2fs (%.1f millones de primos/seg)\n",
            static_cast<unsigned long long>(total_primes),
            static_cast<unsigned long long>(opt.limit),
            total_s,
            total_s > 0 ? (total_primes / 1e6 / total_s) : 0.0);

        return 0;
    }

    // --- Pass 1: byte counting (no I/O) ---
    std::vector<uint64_t> byte_counts(actual_threads, 0);
    std::vector<uint64_t> prime_counts(actual_threads, 0);
    {
        std::atomic<uint64_t> progress{0};
        std::atomic<bool> done{false};
        std::thread prog(print_progress, "contando", std::ref(progress), total_span, std::ref(done));

        std::vector<std::thread> pool;
        for (unsigned i = 0; i < actual_threads; ++i) {
            pool.emplace_back(count_worker, ranges[i], seg_k_width, std::cref(wheel_base_primes),
                               std::ref(byte_counts[i]), std::ref(prime_counts[i]), std::ref(progress));
        }
        for (auto& th : pool) th.join();
        done = true;
        prog.join();
    }

    byte_counts[0] += SMALL_PRIMES_BYTES;
    prime_counts[0] += SMALL_PRIMES_COUNT;

    std::vector<uint64_t> offsets(actual_threads, 0);
    for (unsigned i = 1; i < actual_threads; ++i) offsets[i] = offsets[i - 1] + byte_counts[i - 1];
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
        std::thread prog(print_progress, "escribiendo", std::ref(progress), total_span, std::ref(done));

        std::vector<std::thread> pool;
        for (unsigned i = 0; i < actual_threads; ++i) {
            pool.emplace_back(emit_worker, static_cast<int>(i), ranges[i], seg_k_width,
                               std::cref(wheel_base_primes), fd, offsets[i], std::ref(progress));
        }
        for (auto& th : pool) th.join();
        done = true;
        prog.join();
    }
    ::close(fd);

    auto t_end = std::chrono::steady_clock::now();
    double count_s = std::chrono::duration<double>(t_count_done - t_start).count();
    double write_s = std::chrono::duration<double>(t_end - t_count_done).count();
    double total_s = std::chrono::duration<double>(t_end - t_start).count();

    std::fprintf(stderr,
        "Listo. %llu primos encontrados hasta %llu (%.2f GB).\n"
        "  conteo:     %.2fs\n"
        "  escritura:  %.2fs\n"
        "  total:      %.2fs (%.1f millones de primos/seg)\n",
        static_cast<unsigned long long>(total_primes),
        static_cast<unsigned long long>(opt.limit),
        total_bytes / 1e9,
        count_s, write_s, total_s,
        total_s > 0 ? (total_primes / 1e6 / total_s) : 0.0);

    return 0;
}
