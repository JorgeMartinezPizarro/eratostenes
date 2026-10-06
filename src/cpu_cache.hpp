#pragma once
// CPU cache detection from Linux sysfs (/sys/devices/system/cpu/cpu*/cache),
// and the two sizes the sieve derives from it: the segment width (from the
// L2 share per thread) and the small tier's sub-block (from the L1d). Every
// function returns 0 / empty on failure (non-Linux, no sysfs, odd format);
// callers fall back to conservative defaults, and --l1-bytes/--l2-bytes
// (arg_parser.hpp) override detection by hand.

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "wheel.hpp"

// Best-effort cache size (bytes) of cpu0's cache at a given level (1 = L1
// data, 2 = L2, 3 = L3), from Linux sysfs (also visible inside a Docker
// container running on the host kernel -- Docker Desktop's own VM reports
// a made-up topology instead, see docs/RESEARCH.md). 0 on any failure
// (non-Linux, sysfs unavailable, unexpected format): callers fall back to
// a sane default rather than divide by it. The cpu0 case of
// detect_cpu_cache_info below.
inline uint64_t detect_cache_bytes(int target_level);
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

// A logical CPU's own cache-level total size (bytes) and how many
// logical CPUs share that instance (its "shared_cpu_list" cardinality).
// {0, 0} when the level isn't there or sysfs can't be read; sharers alone
// is 0 when the size was read but the sharing list wasn't.
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

        // shared_cpu_list unreadable or malformed: the size still counts,
        // the share (total / sharers) doesn't -- sharers stays 0.
        std::ifstream shared_f(base + "/shared_cpu_list");
        std::string shared_list;
        int sharers = (shared_f >> shared_list) ? count_cpu_list(shared_list) : 0;
        return {total_bytes, sharers};
    }
    return {};
}

inline uint64_t detect_cache_bytes(int target_level) { return detect_cpu_cache_info(0, target_level).total_bytes; }

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
// sysfs lists. Empty if CPU 0 alone can't be read, so callers can tell
// "no topology here" from "one CPU".
//
// l2_share is the L2 divided by the CPUs that share it (an i5-13500 P-core:
// 1280K / 2 = 640K; an E-core cluster: 2048K / 4 = 512K). l1_raw is the
// L1d undivided: tuning.hpp sizes the sub-block from the raw size and only
// needs the per-CPU values to catch a P-core/E-core difference. l1_sharers
// counts physical cores (SMT siblings share one L1d), for the
// one-thread-per-core rules.
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

// Wheel-index segment width (a multiple of 64) that fills half of
// `l2_bytes` -- the same derivation as parse_args' automatic -s, returning
// the width in wheel indices. tuning.hpp's hybrid step applies this /2 on
// top of an already per-thread L2 share, on purpose (measured faster), see
// docs/RESEARCH.md#seg_k_width_from_l2_bytess-extra-2-margin-applied-on-top-of-an-already-per-thread-l2-share-kept-counterintuitive.
// 0 falls back to 256 KiB.
inline uint64_t seg_k_width_from_l2_bytes(uint64_t l2_bytes) {
    if (l2_bytes == 0) l2_bytes = 256 * 1024;
    uint64_t l2_target_bytes = l2_bytes / 2;
    uint64_t numeric_width = l2_target_bytes * 8 * WHEEL_MOD / WHEEL_SIZE;
    return std::max<uint64_t>(64, (numeric_width * WHEEL_SIZE / WHEEL_MOD) / 64 * 64);
}

// The small tier's sub-block (bytes, a multiple of 8) from a raw L1d size:
// half of it, the per-thread share of an HT pair (tuning.hpp doubles it
// with one thread per core). 0 falls back to 32 KiB. See
// docs/RESEARCH.md#sub-block-size-half-the-l1d-not-all-of-it-kept-2026-09-27.
inline uint64_t sub_block_from_l1_bytes(uint64_t l1_bytes) {
    if (l1_bytes == 0) l1_bytes = 32 * 1024;
    return std::max<uint64_t>(8, l1_bytes / 2 / 8 * 8);
}
