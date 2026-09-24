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
}
