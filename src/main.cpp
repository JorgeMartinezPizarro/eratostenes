// Segmented, parallel, bit-packed Sieve of Eratosthenes on the mod-30 wheel
// (wheel.hpp: one byte = 30 numbers).
//
// Strategy:
//   1. The base primes (<= sqrt(N)), as a bitmap on the wheel
//      (base_sieve.hpp), split into tiers (tuning.hpp).
//   2. The wheel-index range [0, wheel_count_upto(N)) is split into many
//      more chunks than threads; each thread walks its own contiguous run
//      of them, carrying its sieve from chunk to chunk, and steals the back
//      of another run when its own is done (run_parallel_chunks: work isn't
//      uniform across chunks, nor across cores).
//   3. No -o (the default): one pass that only counts.
//   4. Text output (-o *.txt) needs two passes, because pwrite() needs an
//      exact byte offset per chunk up front: a COUNT PASS sizes each
//      chunk's text, prefix sums give the offsets, the file is resized
//      once, and a WRITE PASS re-sieves each chunk and pwrite()s it into
//      its disjoint region, every thread in parallel. Every output byte is
//      written once; the price is sieving twice, which is cheap next to a
//      temp-file-and-merge copy.
//   5. .db output (-o *.db) needs one pass: each thread appends its
//      compressed blocks to the .blk sidecar at offsets handed out by an
//      atomic counter (block_file.hpp), and only the small index rows go
//      through a queue to SQLite's single writer. Each block's position is
//      chunk-relative at first; SqlitePrimeStore::finish corrects it once
//      every chunk's prime count is known.
//
// The wheel's own primes (WHEEL_PRIMES) don't take part in the wheel
// numbering: chunk 0's writer emits them directly (wheel_primes_from).

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
                           uint64_t range_start, uint64_t limit, uint64_t block_size, int zstd_level) {
    SqlitePrimeStore store(path);
    {
        GapBlockSink sink(0, block_size, zstd_level, store.block_file(),
                           [&store](PendingBlock b) { store.push(std::move(b)); });
        for (uint64_t p : primes) sink.write_uint64(p);
        sink.flush();
    }
    store.finish(primes.size(), range_start, limit, WHEEL_MOD, block_size, zstd_level, {});
}

// The wheel's own primes (WHEEL_PRIMES, e.g. 2, 3, 5) at or above `start`:
// they don't take part in the wheel numbering, so the first chunk's writer
// emits them directly. All of them without --start, none for any --start
// past the wheel's last prime.
static std::vector<uint64_t> wheel_primes_from(uint64_t start) {
    std::vector<uint64_t> v;
    for (uint64_t p : WHEEL_PRIMES) if (p >= start) v.push_back(p);
    return v;
}
static std::string wheel_primes_text(uint64_t start) {
    std::string s;
    for (uint64_t p : wheel_primes_from(start)) {
        s += std::to_string(p);
        s += '\n';
    }
    return s;
}

// "up to N", or "in [start, N]" for a --start tail.
static std::string range_desc(uint64_t start, uint64_t limit) {
    return start ? "in [" + format_thousands(start) + ", " + format_thousands(limit) + "]"
                 : "up to " + format_thousands(limit);
}

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
    // start on a word boundary. `start` > 0 (--start, see main) sieves only
    // the tail [start, limit], rounded down to that boundary from
    // wheel_count_upto(start - 1), the index of the first number >= start
    // (the same index SieveConfig::skip_below_k holds).
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
// chunks (begin_chunk() resets the per-chunk state; the presieve fill
// overwrites the segment). Worker threads only live for one pass
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
        slot->sieve->set_range_end(cfg.range_end);
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

// The progress line of one pass: a thread that prints the share of wheel
// indices sieved (k) every 500 ms. The destructor wakes it at once (so the
// pass's reported time doesn't wait for a tick) and joins it, also when a
// worker exception unwinds past it.
class Progress {
public:
    Progress(const Colors& C, const char* label, uint64_t total)
        : th_([this, &C, label, total] { run(C, label, total); }) {}
    ~Progress() {
        { std::lock_guard<std::mutex> lk(mu_); done_ = true; }
        cv_.notify_all();
        th_.join();
    }
    Progress(const Progress&) = delete;
    Progress& operator=(const Progress&) = delete;

    std::atomic<uint64_t> k{0};

private:
    void run(const Colors& C, const char* label, uint64_t total) {
        using namespace std::chrono_literals;
        std::unique_lock<std::mutex> lk(mu_);
        while (!cv_.wait_for(lk, 500ms, [&] { return done_; })) {
            const uint64_t d = k.load(std::memory_order_relaxed);
            const double pct = total ? std::min(100.0, 100.0 * d / total) : 100.0;
            std::fprintf(stderr, "\r  %s%s:%s %s%5.1f%%%s   ", C.label, label, C.reset, C.time, pct, C.reset);
            std::fflush(stderr);
        }
        std::fprintf(stderr, "\r  %s%s:%s %s100.0%%%s   \n", C.label, label, C.reset, C.time, C.reset);
    }

    std::mutex mu_;
    std::condition_variable cv_;
    bool done_ = false;
    std::thread th_; // last: starts once the members above exist
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
// neither are cores (P/E, SMT siblings).
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
    // keep every thread busy.
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
    const SievePlan& sp;
    std::vector<ChunkRange> ranges;
    unsigned actual_threads;
    uint64_t base_limit;
    const Presieve& presieve;
    std::vector<uint64_t> fresh_primes;
    uint64_t total_span;
    uint64_t range_start;
    std::chrono::steady_clock::time_point t_start;

    // One pass over every chunk, with its progress line: fn(i, sieve) for
    // chunk i, where sieve(out) runs the chunk into the sink `out` and
    // returns its prime count.
    template <typename Fn>
    void pass(const char* label, Fn&& fn) const {
        Progress progress(C, label, total_span);
        run_parallel_chunks(actual_threads, ranges, fresh_primes, sp.cfg, [&](unsigned i) {
            const ChunkRange& r = ranges[i];
            fn(i, [&](auto& out) {
                uint64_t count = 0;
                sieve_chunk(r, sp.tiers_for(r), base_limit, presieve, sp.cfg, out, count, progress.k);
                return count;
            });
        });
    }
};

// Count-only (no -o): a single pass, no I/O of any kind.
static int run_count(const RunPlan& plan) {
    const Colors& C = plan.C;
    std::vector<uint64_t> prime_counts(plan.ranges.size(), 0);
    plan.pass("counting", [&](unsigned i, auto sieve) {
        NullSink sink;
        prime_counts[i] = sieve(sink);
    });

    // With --start, only the wheel primes inside [range_start, N] count
    // (none, for any realistic start), so the tail total matches e.g.
    // `primesieve START N -c` exactly.
    uint64_t total_primes = wheel_primes_from(plan.range_start).size();
    for (auto c : prime_counts) total_primes += c;

    auto t_end = std::chrono::steady_clock::now();
    double total_s = std::chrono::duration<double>(t_end - plan.t_start).count();
    double total_mprimes = total_s > 0 ? (total_primes / 1e6 / total_s) : 0.0;

    std::fprintf(stderr,
        "%sDone.%s %s%s%s primes found %s.\n"
        "  %stotal:%s      %s%.3fs%s (%s%.1f M primes/s%s)\n",
        C.headline, C.reset,
        C.bold, format_thousands(total_primes).c_str(), C.reset,
        range_desc(plan.range_start, plan.opt.limit).c_str(),
        C.headline, C.reset, C.time, total_s, C.reset, C.headline, total_mprimes, C.reset);

    return 0;
}

// -o *.db: a single pass. Each chunk's GapBlockSink gap-encodes and
// compresses blocks of db_block_size primes, writes them to the .blk and
// pushes their index rows to the store, with chunk-relative positions that
// finish() corrects from every chunk's prime count.
static int run_db(const RunPlan& plan) {
    const Options& opt = plan.opt;
    const Colors& C = plan.C;
    const unsigned num_chunks = static_cast<unsigned>(plan.ranges.size());
    std::vector<uint64_t> prime_counts(num_chunks, 0);
    SqlitePrimeStore store(opt.output);
    plan.pass("writing", [&](unsigned i, auto sieve) {
        GapBlockSink sink(i, opt.db_block_size, opt.zstd_level, store.block_file(),
                          [&store](PendingBlock b) { store.push(std::move(b)); });
        if (i == 0)
            for (uint64_t p : wheel_primes_from(plan.range_start)) sink.write_uint64(p);
        prime_counts[i] = sieve(sink);
        sink.flush();
    });

    prime_counts[0] += wheel_primes_from(plan.range_start).size();

    std::vector<uint64_t> chunk_offset(num_chunks, 0);
    for (unsigned i = 1; i < num_chunks; ++i) chunk_offset[i] = chunk_offset[i - 1] + prime_counts[i - 1];
    uint64_t total_primes = num_chunks ? chunk_offset.back() + prime_counts.back() : 0;

    store.finish(total_primes, plan.range_start, opt.limit, WHEEL_MOD, opt.db_block_size, opt.zstd_level, chunk_offset);

    auto t_end = std::chrono::steady_clock::now();
    double total_s = std::chrono::duration<double>(t_end - plan.t_start).count();
    double total_mprimes = total_s > 0 ? (total_primes / 1e6 / total_s) : 0.0;
    // The index (.db) plus the blocks (.blk): what the output takes on disk.
    uint64_t db_bytes = fs::file_size(opt.output) + fs::file_size(blk_path_for(opt.output));
    double bytes_per_prime = total_primes ? static_cast<double>(db_bytes) / total_primes : 0.0;

    std::fprintf(stderr,
        "%sDone.%s %s%s%s primes found %s %s(%.2f GB, %.3f B/prime)%s.\n"
        "  %stotal:%s      %s%7.3fs%s  (%s%.1f M primes/s%s)\n",
        C.headline, C.reset,
        C.bold, format_thousands(total_primes).c_str(), C.reset,
        range_desc(plan.range_start, opt.limit).c_str(),
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
    plan.pass("counting", [&](unsigned i, auto sieve) {
        ByteCounter counter;
        prime_counts[i] = sieve(counter);
        byte_counts[i] = counter.total_bytes;
    });

    const std::string head_text = wheel_primes_text(plan.range_start); // written by chunk 0
    byte_counts[0] += head_text.size();
    prime_counts[0] += wheel_primes_from(plan.range_start).size();

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

    // --- Pass 2: every chunk re-sieved and pwrite()n into its own region ---
    try {
        plan.pass("writing", [&](unsigned i, auto sieve) {
            DirectWriter out(fd, offsets[i]);
            if (i == 0) out.write_raw(head_text.data(), head_text.size());
            sieve(out);
            out.flush();
        });
    } catch (...) {
        ::close(fd);
        throw;
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
        "%sDone.%s %s%s%s primes found %s %s(%.2f GB)%s.\n"
        "  %scount:%s      %s%7.3fs%s  (%s%.1f M primes/s%s)\n"
        "  %swrite:%s      %s%7.3fs%s  (%s%.2f GB/s%s)\n"
        "  %stotal:%s      %s%7.3fs%s  (%s%.1f M primes/s%s)\n",
        C.headline, C.reset,
        C.bold, format_thousands(total_primes).c_str(), C.reset,
        range_desc(plan.range_start, opt.limit).c_str(),
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
        // No arguments at all: the help. Otherwise the error alone, with a
        // pointer to the help.
        if (argc == 1) {
            print_usage(argv[0]);
        } else {
            std::fprintf(stderr, "Error: %s\nRun '%s --help' for usage.\n", e.what(), argv[0]);
        }
        return 1;
    }
    if (opt.show_help) {
        print_usage(argv[0]);
        return 0;
    }

    const Colors C(stderr_supports_color());

    auto t_start = std::chrono::steady_clock::now();

    if (opt.limit < 2) {
        if (opt.output.empty()) {
            std::fprintf(stderr, "Done. 0 primes found up to %llu.\n",
                         static_cast<unsigned long long>(opt.limit));
        } else if (is_db_output(opt.output)) {
            write_tiny_db(opt.output, {}, opt.start, opt.limit, opt.db_block_size, opt.zstd_level);
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
        for (uint64_t p : wheel_primes_from(opt.start)) if (opt.limit >= p) small.push_back(p);
        if (opt.output.empty()) {
            std::fprintf(stderr, "Done. %zu prime(s) found %s.\n",
                         small.size(), range_desc(opt.start, opt.limit).c_str());
        } else if (is_db_output(opt.output)) {
            write_tiny_db(opt.output, small, opt.start, opt.limit, opt.db_block_size, opt.zstd_level);
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
    // Built first: the base-prime sieve fills its windows with it too.
    const Presieve presieve = build_presieve(PRESIEVE_GROUPS);
    const BasePrimes base = sieve_base_primes(base_limit, presieve, opt.threads);
    std::fprintf(stderr, "  %llu base primes found.\n", static_cast<unsigned long long>(base.count));

    // Fewer threads when they wouldn't fit the memory budget, before the plan
    // so its per-thread choices (huge arenas, the L3-per-thread cutoff) see
    // the threads that will run.
    const MemCap mem = cap_threads_by_memory(opt, base);
    opt.threads = mem.threads;

    SievePlan P = plan_sieve(opt, base, base_limit);

    // Many more, narrower chunks than threads, so the steals between
    // threads' runs are fine-grained (run_parallel_chunks), but never under
    // one segment each. See
    // docs/RESEARCH.md#run_parallel_chunks-contiguous-runs-the-sieve-carried-across-chunks-steals-kept-2026-10-01.
    constexpr unsigned CHUNKS_PER_THREAD = 150;
    uint64_t width_cap = wheel_count_upto(opt.limit) / P.seg_k_width;
    // No floor of one chunk per thread: a range under threads segments runs
    // on fewer threads (actual_threads below) instead of every thread
    // paying its own setup for a sliver of a segment, as primesieve does.
    unsigned target_chunks = static_cast<unsigned>(std::max<uint64_t>(1,
        std::min<uint64_t>(uint64_t{opt.threads} * CHUNKS_PER_THREAD, width_cap)));
    // --start N0: sieve just [N0, N] instead of [0, N]. Every base prime is
    // still activated for that range, so a tail of a large N costs what the
    // same segments cost in a full run. The count is the tail's, not pi(N);
    // with -o only the tail is written (a .db's positions start at its
    // first prime, meta.range_start = N0).
    uint64_t range_start = opt.start; // below N, parse_args
    if (range_start) {
        std::fprintf(stderr, "WARNING: --start %llu -- only [%llu, %llu] is sieved; the count is NOT pi(N).\n",
                     static_cast<unsigned long long>(range_start), static_cast<unsigned long long>(range_start),
                     static_cast<unsigned long long>(opt.limit));
        // Same chunk width as the full run where that still leaves every
        // thread TAIL_CHUNKS_PER_THREAD chunks; short tails get more,
        // narrower chunks (never under one segment), or the
        // full run's width would leave ~1.5 chunks per thread and cores idle
        // in the last round. Chunks are cheap (a worker carries its sieve
        // through its run): only the steal granularity depends on them.
        constexpr uint64_t TAIL_CHUNKS_PER_THREAD = 32;
        uint64_t span_k = wheel_count_upto(opt.limit) - wheel_count_upto(range_start - 1);
        double frac = static_cast<double>(span_k) / static_cast<double>(wheel_count_upto(opt.limit));
        uint64_t scaled = static_cast<uint64_t>(target_chunks * frac + 0.5);
        uint64_t floor_chunks = std::min<uint64_t>(uint64_t{opt.threads} * TAIL_CHUNKS_PER_THREAD,
                                                   span_k / P.seg_k_width);
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

    print_plan(P, opt, actual_threads, ranges);
    if (mem.capped() || mem.over()) {
        const auto gib = [](uint64_t b) { return static_cast<double>(b) / (1u << 30); };
        const char* why = mem.from_flag ? "--max-mem" : "90% of the available RAM";
        if (mem.capped())
            std::fprintf(stderr, "  memory: %u threads instead of %u (~%.2f GiB each + %.2f GiB shared; budget %.2f GiB, %s)\n",
                         mem.threads, mem.requested, gib(mem.per_thread), gib(mem.shared), gib(mem.budget), why);
        if (mem.over())
            std::fprintf(stderr, "  memory: WARNING: ~%.2f GiB for %u thread(s) is over the budget of %.2f GiB (%s); the run may not fit\n",
                         gib(mem.shared + mem.per_thread * mem.threads), mem.threads, gib(mem.budget), why);
    }

    // Worker exceptions (pwrite() on a full disk, the bucket ring's sizing
    // check) are rethrown by run_parallel_chunks and end here.
    const RunPlan plan{opt, C, P, std::move(ranges), actual_threads, base_limit, presieve,
                       std::move(fresh_primes), total_span, range_start, t_start};
    try {
        if (opt.output.empty()) return run_count(plan);
        if (is_db_output(opt.output)) return run_db(plan);
        return run_text(plan);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "\nError: %s\n", e.what());
        return 1;
    }
}
