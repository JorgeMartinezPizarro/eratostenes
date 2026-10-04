#pragma once
// Command-line argument parsing: sizes with suffixes (k/m/b/t), thread
// count, output path (empty means count-only -- there's no default file,
// see Options::output below), segment width, --tune knobs. The cache
// detection the auto sizes come from lives in cpu_cache.hpp.
//
// The wheel (which primes to skip up front) is NOT here: it's a
// compile-time constant in wheel.hpp, on purpose -- see that file.

#include <cstdint>
#include <string>
#include <stdexcept>
#include <cctype>
#include <cmath>
#include <thread>
#include <fstream>
#include <algorithm>
#include <vector>

#include <zstd.h>

#include "cpu_cache.hpp"
#include "wheel.hpp"


struct Options {
    uint64_t limit = 0;                 // N: sieve up to N (inclusive)
    unsigned threads = 0;                // 0 => auto (hardware_concurrency)
    // Empty (the default) means count-only: no file, no -o needed to just
    // get pi(N). Set via -o/--output; ".db" switches to the compact SQLite
    // format, anything else is plain text.
    std::string output;
    // Numeric width per segment (must be even). 0 means "auto": half the
    // detected L2 (see parse_args), adjusted in tuning.hpp (smallest per-CPU
    // L2 share on hybrids, doubled once the sparse tier exists, power of 2
    // in bytes for the sparse ring). An explicit -s is used as given.
    uint64_t segment_width = 0;
    bool segment_width_set = false;      // true once -s/--segment-width is parsed
    bool show_help = false;

    // Only used when --output ends in ".db" (SQLite + zstd gap encoding,
    // see gap_block_sink.hpp / sqlite_prime_store.hpp). Ignored for plain
    // text output.
    uint64_t db_block_size = 65536;      // primes per compressed block; see
                                          // docs/RESEARCH.md#write-pipeline-knobs-batch---db-block-size-wal_autocheckpoint-all-measured-kept-at-their-defaults
    int zstd_level = 1;                  // 1 beats 3 on both size and time
                                          // with wheel-index gaps. See
                                          // docs/RESEARCH.md#--zstd-level-default-1-kept-2026-09-28

    // Manual overrides for detect_l2_cache_bytes()/detect_l1d_cache_bytes()
    // (this file, below): 0 means "keep auto-detecting". Auto-detection
    // reads /sys/devices/system/cpu/cpu0/cache/index*/ and isn't
    // guaranteed everywhere -- a container runtime, an unusual kernel, or
    // a hybrid P-core/E-core topology can all make it fail silently and
    // fall back to a conservative default sized for a small machine (see
    // where these are used in tuning.hpp/parse_args below), which costs
    // real speed on a bigger machine without saying so anywhere. These
    // flags are the escape hatch when that happens: safe to try because
    // neither touches anything on the per-segment hot path, only how
    // the auto -s width and the small tier's L1 sub-block get sized once
    // at startup.
    uint64_t l2_bytes_override = 0;
    uint64_t l1_bytes_override = 0;

    // Benchmarking aids, count-only. --start N0 sieves just [N0, N]: every
    // base prime is still activated, so a tail of a large N costs what the
    // same segments cost in a full run (main.cpp sizes its chunks so no core
    // idles). --debug-idle prints how far apart the threads finished.
    uint64_t start = 0;
    bool debug_idle = false;

    // --tune key=value (see print_usage): tier cutoffs as fractions of the
    // segment width (den == 0: built-in default), and the switches still
    // under evaluation.
    struct Fraction {
        uint64_t num = 0;
        uint64_t den = 0;
    };
    Fraction tune_small;  // small/med64 cutoff, default 1/4
    Fraction tune_med64;  // med64/medium cutoff, default 1/6 (0 = no med64 tier)
    Fraction tune_sparse; // medium/sparse cutoff, default 1/1, 1/2 or 1/4 (tuning.hpp); in (0, 1]
    Fraction tune_med64s; // sub-blocked med64 band, as a/b of the sub-block (default 0 = off)
    bool big2310 = true; // sparse tier on the mod-2310 wheel (false: mod-210)
    int medium_nta = -1; // medium-tier prefetchnta: -1 auto (L3 gate), 1 on, 0 off
    uint64_t minsegs = 1;    // smallest chunk, in segments (main.cpp's MIN_SEGS_PER_CHUNK)
};

// Interprets suffixes: k=1e3 m=1e6 b=1e9 (short scale billion) t=1e12
// Also accepts scientific notation (1e11, 2.5e15) and plain numbers
// (100000000000). Parsed exactly, in integers: going through a double
// rounded every odd integer above 2^53 (~9.007e15, below the 1e16 target)
// to a neighbour, and llround overflowed past 2^63. The result must be a
// whole number (1.5k is fine, 2.5 is an error) that fits in 64 bits.
inline uint64_t parse_size(const std::string& raw) {
    const auto invalid = [&] { return std::runtime_error("invalid size value: " + raw); };
    const auto out_of_range = [&] { return std::runtime_error("size out of range: " + raw); };
    if (raw.empty()) throw std::runtime_error("empty size value");
    std::string s = raw;
    int exp10 = 0;
    char last = s.back();
    if (std::isalpha(static_cast<unsigned char>(last))) {
        switch (std::tolower(static_cast<unsigned char>(last))) {
            case 'k': exp10 = 3; break;
            case 'm': exp10 = 6; break;
            case 'b': exp10 = 9; break;    // short scale billion (10^9)
            case 'g': exp10 = 9; break;    // giga, alias for b
            case 't': exp10 = 12; break;
            default: throw std::runtime_error("unknown size suffix: " + raw);
        }
        s.pop_back();
    }

    // mantissa digits (integer and fractional part) as one integer
    using u128 = unsigned __int128;
    constexpr u128 MANT_CAP = u128{1} << 120;
    u128 mant = 0;
    bool any_digit = false;
    size_t i = 0;
    for (bool frac = false; i < s.size(); ++i) {
        char c = s[i];
        if (c == '.' && !frac) { frac = true; continue; }
        if (!std::isdigit(static_cast<unsigned char>(c))) break;
        if (mant >= MANT_CAP) throw out_of_range();
        mant = mant * 10 + static_cast<unsigned>(c - '0');
        any_digit = true;
        if (frac) --exp10;
    }
    if (!any_digit) throw invalid();
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
        ++i;
        bool neg = false;
        if (i < s.size() && (s[i] == '+' || s[i] == '-')) neg = s[i++] == '-';
        int e = 0;
        size_t first = i;
        for (; i < s.size() && std::isdigit(static_cast<unsigned char>(s[i])); ++i) {
            if (e > 1000) throw out_of_range();
            e = e * 10 + (s[i] - '0');
        }
        if (i == first) throw invalid();
        exp10 += neg ? -e : e;
    }
    if (i != s.size()) throw invalid();

    for (; exp10 < 0; ++exp10) {
        if (mant % 10 != 0) throw std::runtime_error("size is not a whole number: " + raw);
        mant /= 10;
    }
    for (; exp10 > 0; --exp10) {
        if (mant > UINT64_MAX) throw out_of_range();
        mant *= 10;
    }
    if (mant > UINT64_MAX) throw out_of_range();
    return static_cast<uint64_t>(mant);
}

// Strict signed integer for --zstd-level, within zstd's own range
// (ZSTD_minCLevel()..ZSTD_maxCLevel(), negative levels being zstd's fast
// modes): std::stoi took "abc" as an exception named "stoi" and let any
// integer through to the compressor.
inline int parse_zstd_level(const std::string& v) {
    const size_t digits = v.size() - (!v.empty() && v[0] == '-' ? 1 : 0);
    if (digits == 0 || digits > 7 ||
        !std::all_of(v.begin() + (v.size() - digits), v.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); }))
        throw std::runtime_error("invalid zstd level: " + v);
    const int level = std::stoi(v);
    if (level < ZSTD_minCLevel() || level > ZSTD_maxCLevel())
        throw std::runtime_error("zstd level out of range: " + v + " (" + std::to_string(ZSTD_minCLevel()) + ".." +
                                 std::to_string(ZSTD_maxCLevel()) + ")");
    return level;
}

// Strict unsigned count for -t: std::stoul accepts "-1" and wraps it to
// ULONG_MAX threads.
inline unsigned parse_threads(const std::string& v) {
    if (v.empty() || v.size() > 6 || !std::all_of(v.begin(), v.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); }))
        throw std::runtime_error("invalid thread count: " + v);
    return static_cast<unsigned>(std::stoul(v));
}

// "a/b" or "a" (= a/1), for --tune's cutoff fractions.
inline Options::Fraction parse_fraction(const std::string& key, const std::string& v) {
    Options::Fraction f;
    const size_t slash = v.find('/');
    const std::string n = v.substr(0, slash);
    const std::string d = slash == std::string::npos ? "1" : v.substr(slash + 1);
    try {
        size_t pn = 0, pd = 0;
        f.num = std::stoull(n, &pn);
        f.den = std::stoull(d, &pd);
        if (pn != n.size() || pd != d.size()) throw std::runtime_error("");
    } catch (const std::exception&) {
        throw std::runtime_error("--tune " + key + ": expected an a/b fraction, not '" + v + "'");
    }
    if (f.den == 0) throw std::runtime_error("--tune " + key + ": zero denominator");
    return f;
}

inline bool parse_switch(const std::string& key, const std::string& v, const char* on, const char* off) {
    if (v == on) return true;
    if (v == off) return false;
    throw std::runtime_error("--tune " + key + ": expected " + on + " or " + off + ", not '" + v + "'");
}

inline void parse_tune(Options& opt, const std::string& kv) {
    const size_t eq = kv.find('=');
    if (eq == std::string::npos) throw std::runtime_error("--tune expects key=value, not '" + kv + "'");
    const std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
    if (k == "small") opt.tune_small = parse_fraction(k, v);
    else if (k == "med64") opt.tune_med64 = parse_fraction(k, v);
    else if (k == "sparse") {
        // Lowering only: the dense tiers' state is sized for p < the segment
        // width (SegmentSieve's constructor), so a/b above 1 can't be honoured,
        // and 0 would send every base prime to the bucket ring.
        opt.tune_sparse = parse_fraction(k, v);
        if (opt.tune_sparse.num == 0 || opt.tune_sparse.num > opt.tune_sparse.den)
            throw std::runtime_error("--tune sparse: expected a fraction in (0, 1], not '" + v + "'");
    }
    else if (k == "big2310") opt.big2310 = parse_switch(k, v, "1", "0");
    else if (k == "medium_nta") opt.medium_nta = parse_switch(k, v, "1", "0") ? 1 : 0;
    else if (k == "med64s") opt.tune_med64s = parse_fraction(k, v);
    else if (k == "minsegs") { opt.minsegs = parse_fraction(k, v).num; if (opt.minsegs < 1) throw std::runtime_error("--tune minsegs: at least 1"); }
    else throw std::runtime_error("--tune: unknown key '" + k + "' (small, med64, med64s, sparse, big2310, medium_nta, minsegs)");
}

inline void print_usage(const char* prog) {
    std::fprintf(stderr,
        "Usage: %s N [options]\n"
        "\n"
        "Segmented, parallel Sieve of Eratosthenes. Without -o/--output it\n"
        "only counts the primes up to N (inclusive) -- no file is written.\n"
        "With -o it writes them one per line as plain text, or, if PATH ends\n"
        "in .db, into a compact SQLite file (gaps between consecutive\n"
        "primes, 1-byte encoded and zstd-compressed in blocks), queryable by\n"
        "position with the nth_prime binary.\n"
        "\n"
        "Options:\n"
        "  N                      Upper limit. Accepts k/m/b/t suffixes\n"
        "                         (b = billion = 1e9) and 1e11-style\n"
        "                         notation. E.g. 100b = 1e11.\n"
        "  -o, --output PATH      Output file. Without it, only counts\n"
        "                         (writes nothing). If PATH ends in .db,\n"
        "                         writes SQLite instead of plain text\n"
        "                         (see above).\n"
        "  -t, --threads N        Number of threads (default: available cores)\n"
        "  -s, --segment-width N  Numeric width of each segment\n"
        "                         (default: auto, derived from N and the L2)\n"
        "      --db-block-size N  Primes per compressed block in .db mode\n"
        "                         (default: 65536)\n"
        "      --zstd-level N     zstd compression level in .db mode\n"
        "                         (default: 1)\n"
        "      --l2-bytes N       Force the L2 size used for the automatic\n"
        "                         segment width (default: auto-detected via\n"
        "                         /sys; use it if detection fails, e.g.\n"
        "                         inside a container)\n"
        "      --l1-bytes N       Force the L1 (data) size used for the small\n"
        "                         primes' sub-block (same case as --l2-bytes)\n"
        "  -h, --help             Show this help\n"
        "\n"
        "Benchmarking (count mode only, without -o):\n"
        "      --start N0         Sieve only [N0, N]; the count is that range's,\n"
        "                         not pi(N). E.g. N = 1e15 with\n"
        "                         --start 990e12 is the last 1%%\n"
        "      --debug-idle       Print how far apart the threads finished\n"
        "\n"
        "Fine tuning (--tune key=value, repeatable; see docs/RESEARCH.md):\n"
        "      small=a/b          Small/med64 cutoff (default 1/4 of the segment)\n"
        "      med64=a/b          med64/medium cutoff (default 1/6; 0 = no med64)\n"
        "      med64s=a/b         med64 primes below a/b of the L1 sub-block are\n"
        "                         crossed off per sub-block (default 0 = off; 2/1 to try)\n"
        "      sparse=a/b         Medium/sparse cutoff, in (0, 1] (default 1/1;\n"
        "                         1/2 from 512 KiB and 1/4 from 1 MiB of L2 per\n"
        "                         thread, or 1/2 from 1.5 MiB and 1/4 from 4 MiB of\n"
        "                         L3 per active thread)\n"
        "      big2310=1|0        Sparse tier on the mod-2310 (default 1) or mod-210 wheel\n"
        "      medium_nta=1|0     Force medium-tier prefetchnta on/off (default:\n"
        "                         on once its state outgrows the L3 share)\n"
        "      minsegs=N          Smallest chunk, in segments (default 1; the tail\n"
        "                         granularity between threads at small N)\n"
        "\n"
        "The wheel (which primes are skipped up front) is fixed at compile\n"
        "time in src/wheel.hpp (WHEEL_PRIMES) -- see that file for the\n"
        "prepared configurations and why it isn't a CLI flag.\n"
        "\n"
        "Examples:\n"
        "  %s 1000000 -o primes_1M.txt\n"
        "  %s 100b -o primes_100b.txt -t 12\n"
        "  %s 100b -t 12\n"
        "  %s 100b -o primes_100b.db -t 12\n"
        "  %s 1e15 --start 990e12 --debug-idle\n",
        prog, prog, prog, prog, prog, prog);
}

inline Options parse_args(int argc, char** argv) {
    Options opt;
    auto need_value = [&](int& i, const char* name) -> std::string {
        if (i + 1 >= argc) throw std::runtime_error(std::string("missing value for ") + name);
        return argv[++i];
    };

    bool has_limit = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-h" || a == "--help") {
            opt.show_help = true;
        } else if (a == "-o" || a == "--output") {
            opt.output = need_value(i, a.c_str());
        } else if (a == "-t" || a == "--threads") {
            opt.threads = parse_threads(need_value(i, a.c_str()));
        } else if (a == "-s" || a == "--segment-width") {
            opt.segment_width = parse_size(need_value(i, a.c_str()));
            opt.segment_width_set = true;
        } else if (a == "--db-block-size") {
            opt.db_block_size = parse_size(need_value(i, a.c_str()));
        } else if (a == "--zstd-level") {
            opt.zstd_level = parse_zstd_level(need_value(i, a.c_str()));
        } else if (a == "--l2-bytes") {
            opt.l2_bytes_override = parse_size(need_value(i, a.c_str()));
        } else if (a == "--l1-bytes") {
            opt.l1_bytes_override = parse_size(need_value(i, a.c_str()));
        } else if (a == "--start") {
            opt.start = parse_size(need_value(i, a.c_str()));
        } else if (a == "--debug-idle") {
            opt.debug_idle = true;
        } else if (a == "--tune") {
            parse_tune(opt, need_value(i, a.c_str()));
        } else if (!a.empty() && a[0] != '-' && !has_limit) {
            // Bare positional limit (./eratostenes 1t -c), primesieve-style
            // -- the only way to give it; there's no -n/--limit flag (one
            // less thing to type, matches primesieve's own CLI). Only ever
            // consumes the *first* such argument; a second one falls
            // through to "unknown argument" below instead of silently
            // overwriting the limit.
            opt.limit = parse_size(a);
            has_limit = true;
        } else {
            throw std::runtime_error("unknown argument: " + a);
        }
    }

    if (opt.show_help) return opt;

    if (!has_limit) throw std::runtime_error("missing N (upper limit)");
    // Activation computes p * m up to start + 14p (the sparse tier's mod-2310
    // multiplier moves up to 13 past ceil(start / p): the largest gap between
    // residues coprime to 2310 is 14), with p up to sqrt(N) < 2^32 and start
    // up to N + 30, so N needs 2^32 * 14 + 30 of headroom below 2^64.
    // primesieve's own ceiling, 2^64 - 2^32 * 10, isn't enough: a window just
    // below it threw "bucket sieve: a sparse prime's step exceeds the bucket
    // ring's margin" from a wrapped p * m.
    constexpr uint64_t MAX_LIMIT = UINT64_MAX - 16 * (uint64_t{1} << 32);
    if (opt.limit > MAX_LIMIT)
        throw std::runtime_error("N too large: at most " + std::to_string(MAX_LIMIT) + " (2^64 - 2^32 * 16)");
    // A --start at or past N used to be dropped silently and the whole
    // [0, N] sieved instead.
    if (opt.start >= opt.limit && opt.start != 0)
        throw std::runtime_error("--start must be below N (" + std::to_string(opt.start) + " >= " +
                                 std::to_string(opt.limit) + ")");
    if (opt.threads == 0) {
        opt.threads = std::max(1u, std::thread::hardware_concurrency());
    }
    if (!opt.segment_width_set) {
        // Size the segment to fill half of the machine's actual, detected L2
        // (not guessed) -- falls back to a conservative 256KiB if L2 can't
        // be detected, or use --l2-bytes if that fallback is wrong (see the
        // Options field comment). Past this width some base primes fall
        // into the costlier sparse/bucket tier instead of staying dense --
        // an accepted tradeoff (the bucket ring is pool-allocated, cheap
        // once needed, far cheaper than an L2-blowing segment), not a bug.
        // No longer additionally capped at isqrt(limit) (the smallest width
        // keeping every base prime dense) -- see
        // docs/RESEARCH.md#auto-segment-width-dropping-the-isqrtlimit-cap-kept
        // for why that stopped being the right default, and why the /2
        // itself (not the isqrt cap) stays.
        uint64_t l2_bytes = opt.l2_bytes_override ? opt.l2_bytes_override : detect_l2_cache_bytes();
        if (l2_bytes == 0) l2_bytes = 256 * 1024;
        uint64_t l2_target_bytes = l2_bytes / 2;
        // Inverse of array_bytes = segment_width * WHEEL_SIZE / WHEEL_MOD / 8.
        opt.segment_width = l2_target_bytes * 8 * WHEEL_MOD / WHEEL_SIZE;
    }

    // The segment stays L2-sized (it is also the medium/sparse tier
    // cutoff); L1 residency for the small primes -- the bulk of all marks
    // -- comes from crossing them off one L1d-sized sub-block of the
    // segment at a time instead (see tuning.hpp's sub-block sizing and
    // SegmentSieve::sieve_and_emit), which keeps those two roles decoupled.
    if (opt.segment_width < 64) opt.segment_width = 64;
    if (opt.segment_width % 2 != 0) opt.segment_width += 1; // must be even

    return opt;
}
