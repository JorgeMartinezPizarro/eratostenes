#pragma once
// Command-line argument parsing: sizes with suffixes (k/m/b/t), thread
// count, output path (empty means count-only -- there's no default file,
// see Options::output below), segment width.
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

#include "wheel.hpp"

// Best-effort cache size (bytes) for a given level (1 = L1 data, 2 = L2), Linux
// sysfs (also visible inside a Docker container, since containers share
// the host kernel's /sys). Returns 0 on any failure (non-Linux, sysfs
// unavailable, unexpected format) -- callers must fall back to a sane
// default rather than divide by it directly. Scans
// /sys/devices/system/cpu/cpu0/cache/index*/ for the matching "level" file
// (index numbering isn't standardized -- e.g. index0 is L1d and index2 is
// L2 on this project's own dev machine, but that's not guaranteed
// elsewhere).
inline uint64_t detect_cache_bytes(int target_level) {
    for (int idx = 0; idx < 8; ++idx) {
        std::string base = "/sys/devices/system/cpu/cpu0/cache/index" + std::to_string(idx);
        std::ifstream level_f(base + "/level");
        if (!level_f) break; // no more indices to check
        int level = 0;
        level_f >> level;
        if (level != target_level) continue;
        // L1 is split into Data and Instruction entries; only data counts.
        std::ifstream type_f(base + "/type");
        std::string type;
        if (type_f >> type && type == "Instruction") continue;

        std::ifstream size_f(base + "/size");
        std::string size_str;
        if (!(size_f >> size_str) || size_str.empty()) continue;

        uint64_t mult = 1;
        char suffix = size_str.back();
        if (suffix == 'K' || suffix == 'k') { mult = 1024; size_str.pop_back(); }
        else if (suffix == 'M' || suffix == 'm') { mult = 1024 * 1024; size_str.pop_back(); }
        if (size_str.empty()) continue;

        try {
            size_t pos = 0;
            uint64_t value = std::stoull(size_str, &pos);
            if (pos != size_str.size()) continue;
            return value * mult;
        } catch (const std::exception&) {
            continue;
        }
    }
    return 0;
}
inline uint64_t detect_l2_cache_bytes() { return detect_cache_bytes(2); }
inline uint64_t detect_l1d_cache_bytes() { return detect_cache_bytes(1); }

// How many logical CPUs are named in a Linux sysfs "list" string, e.g.
// "0-1" (2), "12-15,20-23" (8), "5" (1). Used to turn a shared cache's
// raw size into a per-thread share (see detect_cpu_cache_topology below).
// Returns 0 on any parse failure.
inline int count_cpu_list(const std::string& s) {
    int count = 0;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t comma = s.find(',', pos);
        std::string item = s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        if (item.empty()) return 0;
        size_t dash = item.find('-');
        try {
            if (dash == std::string::npos) {
                std::stoi(item);
                count += 1;
            } else {
                int a = std::stoi(item.substr(0, dash));
                int b = std::stoi(item.substr(dash + 1));
                if (b < a) return 0;
                count += (b - a + 1);
            }
        } catch (const std::exception&) {
            return 0;
        }
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return count;
}

// A logical CPU's own EFFECTIVE share (bytes) of a given cache level: that
// cache instance's total size divided by how many logical CPUs actually
// share it (read from its "shared_cpu_list", e.g. "0-1" for a
// hyperthread pair, or "12-15" for 4 E-cores sharing one L2 cluster).
// This is what detect_cache_bytes() (above) can't tell apart on a hybrid
// P-core/E-core CPU: it always reads cpu0, so every thread gets sized for
// cpu0's own cache-sharing situation regardless of which physical core it
// actually lands on. Returns 0 on any failure (same fallback contract as
// detect_cache_bytes).
// A logical CPU's own cache-level total size (bytes) and how many
// logical CPUs share that instance (its "shared_cpu_list" cardinality).
// Returns {0, 0} on any failure (same fallback contract as
// detect_cache_bytes).
struct CpuCacheInfo {
    uint64_t total_bytes = 0;
    int sharers = 0;
};
inline CpuCacheInfo detect_cpu_cache_info(int cpu_id, int target_level) {
    std::string cpu_dir = "/sys/devices/system/cpu/cpu" + std::to_string(cpu_id);
    for (int idx = 0; idx < 8; ++idx) {
        std::string base = cpu_dir + "/cache/index" + std::to_string(idx);
        std::ifstream level_f(base + "/level");
        if (!level_f) break;
        int level = 0;
        level_f >> level;
        if (level != target_level) continue;
        std::ifstream type_f(base + "/type");
        std::string type;
        if (type_f >> type && type == "Instruction") continue;

        std::ifstream size_f(base + "/size");
        std::string size_str;
        if (!(size_f >> size_str) || size_str.empty()) continue;
        uint64_t mult = 1;
        char suffix = size_str.back();
        if (suffix == 'K' || suffix == 'k') { mult = 1024; size_str.pop_back(); }
        else if (suffix == 'M' || suffix == 'm') { mult = 1024 * 1024; size_str.pop_back(); }
        if (size_str.empty()) continue;
        uint64_t total_bytes;
        try {
            size_t pos = 0;
            total_bytes = std::stoull(size_str, &pos) * mult;
            if (pos != size_str.size()) continue;
        } catch (const std::exception&) {
            continue;
        }

        std::ifstream shared_f(base + "/shared_cpu_list");
        std::string shared_list;
        if (!(shared_f >> shared_list)) continue;
        int sharers = count_cpu_list(shared_list);
        if (sharers <= 0) continue;
        return {total_bytes, sharers};
    }
    return {};
}

// A logical CPU's own EFFECTIVE share (bytes) of a given cache level:
// that cache instance's total size divided by how many logical CPUs
// actually share it (e.g. 2 for a hyperthread pair, 4 for an E-core
// cluster). This is what detect_cache_bytes() (above) can't tell apart on
// a hybrid P-core/E-core CPU: it always reads cpu0, so every thread gets
// sized for cpu0's own cache-sharing situation regardless of which
// physical core it actually lands on. Returns 0 on any failure.
inline uint64_t detect_cpu_cache_share(int cpu_id, int target_level) {
    CpuCacheInfo info = detect_cpu_cache_info(cpu_id, target_level);
    if (info.sharers <= 0) return 0;
    return info.total_bytes / static_cast<uint64_t>(info.sharers);
}

// Per-logical-CPU cache sizing, indexed by CPU id, for every online CPU
// sysfs will admit to. Empty (both vectors) if CPU 0 alone can't be read,
// so callers can tell "topology detection isn't available here" from
// "this machine only has one CPU" without a special case: a single-CPU
// vector of size 1 is a valid, if trivial, per-CPU table.
//
// L2 and L1d get different treatment here, on purpose: L2 genuinely is a
// capacity multiple threads draw from at once, so its per-thread fair
// share (l2_share) is total/sharers -- on an i5-13500 (2026-09), cpu0
// (a P-core) reports 1280K/2 sharers = 640K/thread, while an E-core
// cluster reports 2048K/4 sharers = 512K/thread, a ~25% mismatch applied
// uniformly to every E-core thread before this existed. L1d on a
// hyperthread pair isn't reserved/split that way -- SMT time-slices which
// logical thread is actually running, not a strict half held aside up
// front -- and this project's own tuning already settled on using the
// RAW detected L1d size directly with no halving for the single-value
// fallback (see main.cpp's SUB_BLOCK_BYTES comment); l1_raw keeps that
// same, already-validated philosophy per CPU instead of inventing a new
// one, only splitting by CPU to catch a P-core/E-core L1d size difference
// if there is one, not to model HT sharing a second, different way.
// l1_sharers is used for one thing only: counting physical cores (SMT
// siblings share one L1d), so main.cpp can give the sub-block the whole
// L1d instead of half when there are no more threads than cores.
struct CpuCacheTopology {
    std::vector<uint64_t> l1_raw;   // index = logical CPU id, 0 = undetected
    std::vector<int> l1_sharers;    // logical CPUs on that L1d, 0 = undetected
    std::vector<uint64_t> l2_share;
};
inline CpuCacheTopology detect_cpu_cache_topology() {
    CpuCacheTopology topo;
    for (int cpu = 0; ; ++cpu) {
        std::ifstream probe("/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/cache/index0/level");
        if (!probe) break;
        CpuCacheInfo l1 = detect_cpu_cache_info(cpu, 1);
        topo.l1_raw.push_back(l1.total_bytes);
        topo.l1_sharers.push_back(l1.sharers);
        topo.l2_share.push_back(detect_cpu_cache_share(cpu, 2));
    }
    if (topo.l1_raw.empty() || topo.l1_raw[0] == 0) return {};
    return topo;
}

// Wheel-index segment width (word-aligned to 64, ready for SegmentSieve)
// that fills half of `l2_bytes` -- same derivation as the auto -s formula
// below, but returning k-width directly. Also used in main.cpp's
// per-CPU-minimum step, deliberately applying this same /2 margin even on
// top of an already-per-thread L2 share -- counterintuitive, see
// docs/RESEARCH.md#seg_k_width_from_l2_bytess-extra-2-margin-applied-on-top-of-an-already-per-thread-l2-share-kept-counterintuitive.
// 0 falls back to a conservative 256KiB.
inline uint64_t seg_k_width_from_l2_bytes(uint64_t l2_bytes) {
    if (l2_bytes == 0) l2_bytes = 256 * 1024;
    uint64_t l2_target_bytes = l2_bytes / 2;
    uint64_t numeric_width = l2_target_bytes * 8 * WHEEL_MOD / WHEEL_SIZE;
    return std::max<uint64_t>(64, (numeric_width * WHEEL_SIZE / WHEEL_MOD) / 64 * 64);
}

// L1-sized sub-block (bytes, word-aligned to 8) for the small tier -- same
// derivation as main.cpp's SUB_BLOCK_BYTES, callable per-CPU with a RAW
// (undivided -- see CpuCacheTopology's comment on l1_raw) L1d size. 0
// falls back to the same conservative 32KiB the global path uses.
// Half the L1d, not all of it: leaves room in L1 for the small tier's own
// state and the presieve window alongside the sub-block. See
// docs/RESEARCH.md#sub-block-size-half-the-l1d-not-all-of-it-kept-2026-09-27.
inline uint64_t sub_block_from_l1_bytes(uint64_t l1_bytes) {
    if (l1_bytes == 0) l1_bytes = 32 * 1024;
    return std::max<uint64_t>(8, l1_bytes / 2 / 8 * 8);
}

struct Options {
    uint64_t limit = 0;                 // N: sieve up to N (inclusive)
    unsigned threads = 0;                // 0 => auto (hardware_concurrency)
    // Empty (the default) means count-only: no file, no -o needed to just
    // get pi(N). Set via -o/--output; ".db" switches to the compact SQLite
    // format, anything else is plain text.
    std::string output;
    // Numeric width per segment (must be even). 0 means "auto": half the
    // detected L2 (see parse_args), adjusted in main.cpp (smallest per-CPU
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
    // where these are used in main.cpp/parse_args below), which costs
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
    Fraction tune_med64;  // med64/medium cutoff, default 1/12 (0 = no med64 tier)
    Fraction tune_sparse; // medium/sparse cutoff, default 1/1 or 1/2 (main.cpp); lowering only
    bool big2310 = true; // sparse tier on the mod-2310 wheel (false: mod-210)
    int medium_nta = -1; // medium-tier prefetchnta: -1 auto (L3 gate), 1 on, 0 off
    uint64_t minsegs = 4;    // smallest chunk, in segments (main.cpp's MIN_SEGS_PER_CHUNK)
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
    else if (k == "sparse") opt.tune_sparse = parse_fraction(k, v);
    else if (k == "big2310") opt.big2310 = parse_switch(k, v, "1", "0");
    else if (k == "medium_nta") opt.medium_nta = parse_switch(k, v, "1", "0") ? 1 : 0;
    else if (k == "minsegs") { opt.minsegs = parse_fraction(k, v).num; if (opt.minsegs < 1) throw std::runtime_error("--tune minsegs: at least 1"); }
    else throw std::runtime_error("--tune: unknown key '" + k + "' (small, med64, sparse, big2310, medium_nta, minsegs)");
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
        "      med64=a/b          med64/medium cutoff (default 1/12; 0 = no med64)\n"
        "      sparse=a/b         Medium/sparse cutoff, lowering only\n"
        "                         (default 1/1, or 1/2 with >= 512 KiB L2 per thread)\n"
        "      big2310=1|0        Sparse tier on the mod-2310 (default 1) or mod-210 wheel\n"
        "      medium_nta=1|0     Force medium-tier prefetchnta on/off (default:\n"
        "                         on once its state outgrows the L3 share)\n"
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
            opt.zstd_level = std::stoi(need_value(i, a.c_str()));
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
    // segment at a time instead (see main.cpp's SUB_BLOCK_BYTES and
    // SegmentSieve::sieve_and_emit), which keeps those two roles decoupled.
    if (opt.segment_width < 64) opt.segment_width = 64;
    if (opt.segment_width % 2 != 0) opt.segment_width += 1; // must be even

    return opt;
}
