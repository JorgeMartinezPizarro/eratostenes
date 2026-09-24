#pragma once
// EratBig-style tables: mod-210 multiplier wheel over the mod-30 byte layout.
//
// Sparse-tier primes (p >= seg_k_width, at most ~1 hit/segment) only need
// multipliers coprime to 210 = 2*3*5*7, not the full set coprime to 30:
// any p*m where m is a multiple of 7 lands on a composite that's ALSO a
// multiple of 7, which 7's own (small-tier) crossing-off already covers
// across the whole range -- so those multiplier phases are simply
// redundant work for this tier specifically, never a correctness gap.
// 48/210 phases instead of 8/30 is ~14% fewer multiplier candidates per
// prime (primesieve's own EratBig does the same for the same reason).
#include <array>
#include <cstdint>
namespace big {
constexpr uint32_t R30[8] = {1, 7, 11, 13, 17, 19, 23, 29};
constexpr int pos30(uint32_t x) { for (int j = 0; j < 8; ++j) if (R30[j] == x) return j; return -1; }
constexpr std::array<uint32_t, 48> make_m210() {
    std::array<uint32_t, 48> m{}; int n = 0;
    for (uint32_t i = 1; i < 210; ++i) if (i % 2 && i % 3 && i % 5 && i % 7) m[n++] = i;
    return m;
}
constexpr std::array<uint32_t, 48> M210 = make_m210();
// Per (residue class ri, multiplier phase w) entry: mask/byte-step/exit
// phase for stepping p's hits one 210-wheel phase at a time. mask marks
// bit pos30((r*m)%30) of the CURRENT hit; dm/corr give the byte distance
// to the NEXT hit (m -> next 210-coprime multiplier): byte step =
// qp*dm + corr, where corr = floor(r*m2/30) - floor(r*m/30) (derived from
// r*dm = 30*corr + ((r*m2)%30 - (r*m)%30), i.e.
// corr = (r*dm + (r*m)%30 - (r*m2)%30) / 30).
struct Entry { uint8_t mask; uint8_t dm; uint8_t corr; uint8_t pad; uint16_t next; uint16_t pad2; };
constexpr std::array<Entry, 384> make_table() {
    std::array<Entry, 384> t{};
    for (int ri = 0; ri < 8; ++ri) for (int w = 0; w < 48; ++w) {
        uint32_t r = R30[ri];
        uint32_t m = M210[w];
        uint32_t m2 = (w == 47) ? M210[0] + 210 : M210[w + 1];
        uint32_t dm = m2 - m;
        uint32_t c = (r * dm + (r * m) % 30 - (r * m2) % 30) / 30;
        t[ri * 48 + w] = {static_cast<uint8_t>(1u << pos30((r * m) % 30)), static_cast<uint8_t>(dm),
                          static_cast<uint8_t>(c), 0, static_cast<uint16_t>(ri * 48 + (w + 1) % 48), 0};
    }
    return t;
}
inline constexpr std::array<Entry, 384> TABLE = make_table();
// first index w with M210[w] >= s (s in [0,210]), or 48 if none
constexpr std::array<uint8_t, 211> make_next() {
    std::array<uint8_t, 211> a{};
    for (int s = 0; s <= 210; ++s) { int w = 0; while (w < 48 && M210[w] < static_cast<uint32_t>(s)) ++w; a[s] = static_cast<uint8_t>(w); }
    return a;
}
inline constexpr std::array<uint8_t, 211> NEXT_W = make_next();

// Wheel-index (k-space, bit granularity) stepping tables for the medium
// tier's mod-210 multiplier stepping (erat_small.hpp::cross_off_medium).
// Every medium-tier prime is > 163 (presieve's own {7,23,37} group always
// covers 7 first, see presieve.hpp), so any hit whose multiplier is a
// multiple of 7 lands on a composite 7 already marked -- pure redundant
// work for this tier specifically, same reasoning as the sparse tier's own
// TABLE above (that one is byte-granularity for the bucket sieve; this one
// is bit-granularity for the medium tier's flat one-hit-per-iteration
// loop, but it's the same 48/210-vs-8/30 saving, ~14% fewer candidate
// hits).
//
// Same delta split as wheel.hpp's ONFLY_CORRECTION/GAP_K (delta = qp *
// gap_k + corr), re-derived with M210 (48 mod-210-coprime multiplier
// phases) standing in for WHEEL_R (8 mod-30-coprime phases): writing
// p = qp*30 + R30[ri] and letting a hit's multiplier step from M210[w] to
// M210[w+1] (mod 210, +210 on wraparound), the wheel-index advance is
//   delta = qp*30 * gap210(w) + R30[ri]*gap210(w)   [n advances by p*gap210(w)]
// and the first term is an exact multiple of 30, so (exactly like
// wheel_delta_at's own derivation) it becomes qp*(gap210(w)*8) plus a
// (ri, w)-only correction -- independent of qp, i.e. independent of the
// prime's actual magnitude. Both terms are tiny, shared, read-only tables
// (48 + 8*48 entries) -- unlike the reverted 64-list attempt
// (erat_small.hpp), this doesn't touch DenseState's layout or split the
// medium tier's own flat list at all, so there's no per-prime memory or
// locality cost to trade against the instruction savings.
//
// Deliberately kept as two flat arrays indexed by (ri fixed per prime,
// outside the loop) and w (a plain incrementing loop variable), NOT as one
// struct-with-a-"next"-field table indexed by a `w`/`idx` that's itself
// loaded from the previous lookup: a first version did that (mirroring
// big::TABLE/Entry above, which the byte-marking sparse tier can afford
// since it's only ever called once per prime per *segment*), and it
// regressed cycles:u despite ~15% fewer instructions:u -- the "next"
// field's load-to-use chain serializes one table load behind the previous
// one every hit, whereas the old mod-30 code's `j = (j + 1) & 7` (and this
// version's `w` wraparound) is pure register arithmetic with no such
// dependency, so independent loop iterations' table loads can issue
// without waiting on each other. Measured (dev PC, i5-11400F, perf stat
// cycles:u, N=1e12 natural auto -s): "next"-field version 1.5105T ->
// 1.5453T cycles:u (+2.3%, regression) despite instructions:u 2.002T ->
// 1.700T (-15.1%, roughly the expected ~14% hit reduction) -- IPC dropped
// 1.33 -> 1.10, confirming a latency, not throughput, problem. Reverted to
// this two-array form before it was ever committed.
constexpr std::array<uint32_t, 48> make_gap_k210() {
    std::array<uint32_t, 48> g{};
    for (int w = 0; w < 48; ++w) {
        uint32_t m1 = M210[w];
        uint32_t m2 = (w == 47) ? M210[0] + 210 : M210[w + 1];
        g[w] = (m2 - m1) * 8;
    }
    return g;
}
inline constexpr std::array<uint32_t, 48> GAP_K210 = make_gap_k210();

constexpr std::array<std::array<uint32_t, 48>, 8> make_onfly_correction210() {
    std::array<std::array<uint32_t, 48>, 8> tbl{};
    for (int ri = 0; ri < 8; ++ri) {
        uint32_t p_mod30 = R30[ri];
        for (int w = 0; w < 48; ++w) {
            uint32_t m1 = M210[w];
            uint32_t m2 = (w == 47) ? M210[0] + 210 : M210[w + 1];
            uint32_t gap210 = m2 - m1;
            uint32_t rp = (p_mod30 * (m1 % 30)) % 30;
            uint64_t d = static_cast<uint64_t>(p_mod30) * gap210;
            uint64_t c1 = (rp + d) / 30;
            uint32_t rem = static_cast<uint32_t>((rp + d) % 30);
            int pos_before = pos30(rp);
            int pos_after = pos30(rem);
            int64_t corr = static_cast<int64_t>(c1) * 8 + (pos_after - pos_before);
            tbl[ri][w] = static_cast<uint32_t>(corr);
        }
    }
    return tbl;
}
inline constexpr std::array<std::array<uint32_t, 48>, 8> ONFLY_CORRECTION210 = make_onfly_correction210();
}
