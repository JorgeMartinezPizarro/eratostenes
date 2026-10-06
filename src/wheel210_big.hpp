#pragma once
// EratBig-style tables: mod-210 multiplier wheel over the mod-30 byte layout.
//
// The med64, medium and sparse tiers (every prime > 163) only need
// multipliers coprime to 210 = 2*3*5*7, not the full set coprime to 30:
// any p*m where m is a multiple of 7 lands on a composite that's ALSO a
// multiple of 7, which the presieve pattern (presieve.hpp) already marks
// across the whole range -- so those multiplier phases are simply
// redundant work, never a correctness gap. 48/210 phases instead of 8/30
// is ~14% fewer multiplier candidates per prime (primesieve's own EratBig
// does the same for the same reason).
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

// Mod-2310 multiplier wheel for the sparse tier (SegmentSieve::process_big): 11
// is presieved too (presieve.hpp), so multipliers that are multiples of 11
// only re-mark composites the presieve pattern already has -- 480/2310 phases
// instead of 48/210, ~9.1% fewer sparse hits.
constexpr uint32_t W2310 = 480;
constexpr std::array<uint16_t, W2310> make_m2310() {
    std::array<uint16_t, W2310> m{}; uint32_t n = 0;
    for (uint32_t i = 1; i < 2310; ++i) if (i % 2 && i % 3 && i % 5 && i % 7 && i % 11) m[n++] = static_cast<uint16_t>(i);
    if (n != W2310) throw "M2310: phase count";
    return m;
}
inline constexpr std::array<uint16_t, W2310> M2310 = make_m2310();
// first index w with M2310[w] >= s (s in [0,2310]), or W2310 if none
constexpr std::array<uint16_t, 2311> make_next2310() {
    std::array<uint16_t, 2311> a{}; uint32_t w = 0;
    for (uint32_t s = 0; s <= 2310; ++s) { while (w < W2310 && M2310[w] < s) ++w; a[s] = static_cast<uint16_t>(w); }
    return a;
}
inline constexpr std::array<uint16_t, 2311> NEXT_W2310 = make_next2310();
// Per (residue class ri, phase w), index ri * 480 + w, packed in 32 bits
// (15 KiB): mask | dm << 8 | corr << 16 | next << 20. Same mask/dm/corr
// meaning as TABLE (dm <= 14, corr <= 14); next is the following phase's
// index, wrapping at w == 479. A 16-bit row (dm/2 and a wrap bit, next
// computed as idx + 1 - wrap * 480) was tried first: 43 instructions per
// hit instead of mod-210's 37, eating part of the ~9% fewer hits.
constexpr std::array<uint32_t, 8 * W2310> make_table2310() {
    std::array<uint32_t, 8 * W2310> t{};
    for (uint32_t ri = 0; ri < 8; ++ri) for (uint32_t w = 0; w < W2310; ++w) {
        uint32_t r = R30[ri];
        uint32_t m = M2310[w];
        uint32_t m2 = (w == W2310 - 1) ? M2310[0] + 2310u : M2310[w + 1];
        uint32_t dm = m2 - m;
        uint32_t c = (r * dm + (r * m) % 30 - (r * m2) % 30) / 30;
        if (dm > 255 || c > 15) throw "TABLE2310: field overflow";
        uint32_t next = ri * W2310 + (w + 1) % W2310;
        t[ri * W2310 + w] = (1u << pos30((r * m) % 30)) | (dm << 8) | (c << 16) | (next << 20);
    }
    return t;
}
inline constexpr std::array<uint32_t, 8 * W2310> TABLE2310 = make_table2310();

// Medium-tier stepping table (erat_small.hpp::cross_off_medium): the same
// mod-210 multiplier wheel and byte layout as TABLE above, packed as
// PACK210[ri][w] = mask | dm << 8 | corr << 16, so a medium hit is
// `s[pos] |= mask` and the next one is `qp * dm + corr` bytes further. Every
// medium prime is > 163 and 7 is always presieved, so skipping multipliers
// that are multiples of 7 saves ~14% of this tier's hits.
//
// Indexed by (ri, fixed per prime) and w, a plain register loop variable --
// NOT through TABLE's "next" field, whose load-to-use chain serializes one
// lookup behind the previous one every hit. One packed word instead of three
// arrays: the loop was bound by load ports (4 loads per hit with the
// store's own), and the extra shifts go to otherwise idle ALU ports -- see
// docs/RESEARCH.md. Two full 48-phase cycles, so the loop can run w past 47
// without wrapping it on every hit.
constexpr std::array<std::array<uint32_t, 96>, 8> make_pack210() {
    std::array<std::array<uint32_t, 96>, 8> a{};
    for (int ri = 0; ri < 8; ++ri) for (int w = 0; w < 96; ++w) {
        const Entry& e = TABLE[ri * 48 + w % 48];
        a[ri][w] = uint32_t{e.mask} | (uint32_t{e.dm} << 8) | (uint32_t{e.corr} << 16);
    }
    return a;
}
inline constexpr std::array<std::array<uint32_t, 96>, 8> PACK210 = make_pack210();
}
