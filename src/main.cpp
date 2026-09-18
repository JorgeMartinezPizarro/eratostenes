// Segmented, parallel, bit-packed Sieve of Eratosthenes, on a wheel that
// skips multiples of a small fixed set of primes up front (WHEEL_PRIMES in
// wheel.hpp -- see that file for why bigger isn't always better here, and
// why it's a compile-time constant instead of a CLI flag).
//
// Strategy:
//   1. Compute the base primes (<= sqrt(N)) with a simple sieve.
//   2. The "wheel index" range [1, wheel_count_upto(N)) -- which enumerates
//      the numbers coprime with WHEEL_MOD above the wheel's own primes, see
//      wheel.hpp -- is split into T contiguous chunks, one per thread.
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
#include "colors.hpp"
#include "presieve.hpp"
#include "segment_sieve.hpp"
#include "sinks.hpp"
#include "wheel.hpp"

namespace fs = std::filesystem;

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
                         const std::vector<uint64_t>& sparse_primes,
                         const Presieve& presieve,
                         Writer& out, uint64_t& local_count,
                         std::atomic<uint64_t>& progress) {
    SegmentSieve sieve(seg_k_width, base_prime_max, presieve);
    sieve.begin_chunk();
    for (uint64_t k_low = range.low; k_low < range.high; k_low += seg_k_width) {
        uint64_t k_high = std::min(k_low + seg_k_width, range.high);
        sieve.sieve_and_emit(k_low, k_high, wheel_base_primes, sparse_primes, out, local_count);
        progress.fetch_add(k_high - k_low, std::memory_order_relaxed);
    }
}

// Count-only pass: no I/O, no byte accounting, just the prime count.
static void count_only_worker(ChunkRange range, uint64_t seg_k_width, uint64_t base_prime_max,
                               const std::vector<WheelBasePrime>& wheel_base_primes,
                               const std::vector<uint64_t>& sparse_primes,
                               const Presieve& presieve,
                               uint64_t& out_count, std::atomic<uint64_t>& progress) {
    NullSink sink;
    uint64_t local_count = 0;
    sieve_chunk(range, seg_k_width, base_prime_max, wheel_base_primes, sparse_primes, presieve, sink, local_count, progress);
    out_count = local_count;
}

// Byte-counting pass: no disk I/O, just measures how many text bytes each
// thread's primes will take.
static void count_worker(ChunkRange range, uint64_t seg_k_width, uint64_t base_prime_max,
                          const std::vector<WheelBasePrime>& wheel_base_primes,
                          const std::vector<uint64_t>& sparse_primes,
                          const Presieve& presieve,
                          uint64_t& out_bytes, uint64_t& out_count,
                          std::atomic<uint64_t>& progress) {
    ByteCounter counter;
    uint64_t local_count = 0;
    sieve_chunk(range, seg_k_width, base_prime_max, wheel_base_primes, sparse_primes, presieve, counter, local_count, progress);
    out_bytes = counter.total_bytes;
    out_count = local_count;
}

// Write pass: re-sieves the same chunk and writes with pwrite() directly
// into its (disjoint) region of the final file.
static void emit_worker(int idx, ChunkRange range, uint64_t seg_k_width, uint64_t base_prime_max,
                         const std::vector<WheelBasePrime>& wheel_base_primes,
                         const std::vector<uint64_t>& sparse_primes,
                         const Presieve& presieve,
                         int fd, uint64_t base_offset,
                         std::atomic<uint64_t>& progress) {
    DirectWriter out(fd, base_offset);
    uint64_t local_count = 0;

    if (idx == 0) {
        out.write_raw(SMALL_PRIMES_TEXT.data(), SMALL_PRIMES_BYTES);
    }

    sieve_chunk(range, seg_k_width, base_prime_max, wheel_base_primes, sparse_primes, presieve, out, local_count, progress);
    out.flush();
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
        unsigned n = 0;
        for (uint64_t p : WHEEL_PRIMES) if (opt.limit >= p) ++n;
        if (opt.count_only) {
            std::fprintf(stderr, "Listo. %u primo(s) encontrado(s) hasta %llu.\n",
                         n, static_cast<unsigned long long>(opt.limit));
        } else {
            std::ofstream ofs(opt.output, std::ios::binary | std::ios::trunc);
            for (uint64_t p : WHEEL_PRIMES) if (opt.limit >= p) ofs << p << "\n";
            std::fprintf(stderr, "Listo. %u primo(s) escrito(s) en %s\n", n, opt.output.c_str());
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

    // Per-prime wheel jump table (the wheel's own primes don't need one:
    // they are special-cased). Computed once here, not once per segment.
    // Primes also covered by the pre-sieve pattern (see presieve.hpp) are
    // skipped here: they're never scheduled as active markers, their
    // multiples come pre-marked from the pattern buffer instead. They
    // still come out as output/count -- nothing marks the primes
    // themselves composite either way, so they survive extraction exactly
    // as before.
    //
    // The rest split dense/sparse (see segment_sieve.hpp): a prime's
    // average hit-gap in wheel-index terms is ~p itself, so p >=
    // seg_k_width means at most ~1 hit per segment -- not enough repeats
    // within a segment for the per-phase delta table to earn its keep.
    // Those primes skip the table (sparse_primes, just the plain uint64_t)
    // instead of paying wheel.hpp's `8 + 4*phi(wheel)` bytes each.
    std::vector<WheelBasePrime> wheel_base_primes;
    std::vector<uint64_t> sparse_primes;
    wheel_base_primes.reserve(base_primes.size());
    for (uint64_t p : base_primes) {
        if (p < FIRST_WHEEL_PRIME) continue;
        if (std::find(PRESIEVE_PRIMES.begin(), PRESIEVE_PRIMES.end(), p) != PRESIEVE_PRIMES.end()) continue;
        if (p < seg_k_width) {
            wheel_base_primes.push_back({p, compute_wheel_deltas(p)});
        } else {
            sparse_primes.push_back(p);
        }
    }

    auto ranges = split_ranges(opt.limit, opt.threads);
    unsigned actual_threads = static_cast<unsigned>(ranges.size());

    uint64_t total_span = ranges.back().high - ranges.front().low;

    Presieve presieve = build_presieve(PRESIEVE_PRIMES, seg_k_width);

    std::fprintf(stderr, "Iniciando %u hilos, limite=%llu, segmento=%llu, rueda mod %llu (%zu primos), "
                 "%zu primos base densos, %zu dispersos...\n",
                 actual_threads,
                 static_cast<unsigned long long>(opt.limit),
                 static_cast<unsigned long long>(opt.segment_width),
                 static_cast<unsigned long long>(WHEEL_MOD),
                 WHEEL_PRIMES.size(),
                 wheel_base_primes.size(), sparse_primes.size());

    if (opt.count_only) {
        // Single pass, no I/O of any kind: NullSink skips even the
        // to_chars conversion, since we only need the running count that
        // sieve_and_emit already tracks internally.
        std::vector<uint64_t> prime_counts(actual_threads, 0);
        std::atomic<uint64_t> progress{0};
        std::atomic<bool> done{false};
        std::thread prog(print_progress, std::cref(C), "contando", std::ref(progress), total_span, std::ref(done));

        std::vector<std::thread> pool;
        for (unsigned i = 0; i < actual_threads; ++i) {
            pool.emplace_back(count_only_worker, ranges[i], seg_k_width, base_limit, std::cref(wheel_base_primes),
                               std::cref(sparse_primes), std::cref(presieve), std::ref(prime_counts[i]), std::ref(progress));
        }
        for (auto& th : pool) th.join();
        done = true;
        prog.join();

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

    // --- Pass 1: byte counting (no I/O) ---
    std::vector<uint64_t> byte_counts(actual_threads, 0);
    std::vector<uint64_t> prime_counts(actual_threads, 0);
    {
        std::atomic<uint64_t> progress{0};
        std::atomic<bool> done{false};
        std::thread prog(print_progress, std::cref(C), "contando", std::ref(progress), total_span, std::ref(done));

        std::vector<std::thread> pool;
        for (unsigned i = 0; i < actual_threads; ++i) {
            pool.emplace_back(count_worker, ranges[i], seg_k_width, base_limit, std::cref(wheel_base_primes),
                               std::cref(sparse_primes), std::cref(presieve), std::ref(byte_counts[i]), std::ref(prime_counts[i]), std::ref(progress));
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
        std::thread prog(print_progress, std::cref(C), "escribiendo", std::ref(progress), total_span, std::ref(done));

        std::vector<std::thread> pool;
        for (unsigned i = 0; i < actual_threads; ++i) {
            pool.emplace_back(emit_worker, static_cast<int>(i), ranges[i], seg_k_width, base_limit,
                               std::cref(wheel_base_primes), std::cref(sparse_primes), std::cref(presieve),
                               fd, offsets[i], std::ref(progress));
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
}
