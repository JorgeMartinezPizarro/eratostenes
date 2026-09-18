#pragma once
// Wheel factorization: skips multiples of a small fixed set of primes up
// front. WHEEL_PRIMES below is a compile-time constant: to try a different
// wheel, uncomment one of the labeled configs and recompile (`make`). The
// wheel is compile-time (not a runtime flag) so the compiler can turn a
// division by the wheel's modulus into a cheap multiply-shift, which it
// can't do for a runtime value.
//
// Bigger wheels remove more candidates per number checked, but the
// per-prime jump table (see WheelBasePrime below) needed to do that grows
// faster than the benefit: adding prime p multiplies the table by (p-1)
// but only cuts marking work by (p-1)/p. Past a certain size that table
// stops fitting the CPU's L3 cache and the sieve becomes
// memory-bandwidth-bound rather than compute-bound. See BENCHMARK.md for
// measured table sizes and timings across wheels and N.
//
//   Some configs to try (uncomment one, comment the rest, then `make`):
//
//     constexpr std::array<uint64_t, 2> WHEEL_PRIMES = {2, 3};             // mod 6
//     constexpr std::array<uint64_t, 3> WHEEL_PRIMES = {2, 3, 5};          // mod 30
//     constexpr std::array<uint64_t, 4> WHEEL_PRIMES = {2, 3, 5, 7};       // mod 210
//     constexpr std::array<uint64_t, 5> WHEEL_PRIMES = {2, 3, 5, 7, 11};   // mod 2310
//
// The array size (the std::array<uint64_t, N> template argument) must
// match the number of primes listed.

#include <cstdint>
#include <array>

//     constexpr std::array<uint64_t, 2> WHEEL_PRIMES = {2, 3};             // mod 6
     constexpr std::array<uint64_t, 3> WHEEL_PRIMES = {2, 3, 5};          // mod 30
//     constexpr std::array<uint64_t, 4> WHEEL_PRIMES = {2, 3, 5, 7};       // mod 210
//     constexpr std::array<uint64_t, 5> WHEEL_PRIMES = {2, 3, 5, 7, 11};   // mod 2310

constexpr uint64_t wheel_gcd(uint64_t a, uint64_t b) {
    while (b != 0) {
        uint64_t t = b;
        b = a % b;
        a = t;
    }
    return a;
}

constexpr uint64_t compute_wheel_mod() {
    uint64_t m = 1;
    for (uint64_t p : WHEEL_PRIMES) m *= p;
    return m;
}
constexpr uint64_t WHEEL_MOD = compute_wheel_mod();

constexpr int compute_wheel_size() {
    // WHEEL_MOD is squarefree (product of distinct primes), so
    // phi(WHEEL_MOD) = product(p - 1) over its prime factors.
    uint64_t phi = 1;
    for (uint64_t p : WHEEL_PRIMES) phi *= (p - 1);
    return static_cast<int>(phi);
}
constexpr int WHEEL_SIZE = compute_wheel_size();

// Whether WHEEL_SIZE is a power of 2 (true for 2,3 -> 2 and 2,3,5 -> 8;
// false for 2,3,5,7 -> 48 and 2,3,5,7,11 -> 480). When it is, phase
// wraparound (j -> (j+1) mod WHEEL_SIZE) can use a mask instead of a
// compare-and-reset branch -- see segment_sieve.hpp, via `if constexpr`.
constexpr bool WHEEL_SIZE_IS_POW2 = (WHEEL_SIZE & (WHEEL_SIZE - 1)) == 0;

constexpr int compute_wheel_size_log2() {
    int v = WHEEL_SIZE;
    int log = 0;
    while (v > 1) { v >>= 1; ++log; }
    return log;
}
// 0 (not -1) when not a power of 2: that branch is never taken at runtime
// (see the `if constexpr` in segment_sieve.hpp), but since WHEEL_SIZE_IS_POW2
// isn't template-dependent there, the compiler still type-checks the
// discarded branch, and a negative shift count would warn (UB if it were
// ever evaluated, even though it never is).
constexpr int WHEEL_SIZE_LOG2 = WHEEL_SIZE_IS_POW2 ? compute_wheel_size_log2() : 0;

constexpr bool is_prime_trial(uint64_t n) {
    if (n < 2) return false;
    for (uint64_t d = 2; d * d <= n; ++d) {
        if (n % d == 0) return false;
    }
    return true;
}

// The smallest prime not covered by the wheel -- the first "base prime"
// that actually needs marking (below it, primes are emitted directly).
constexpr uint64_t compute_first_wheel_prime() {
    uint64_t c = WHEEL_PRIMES.back() + 1;
    while (!is_prime_trial(c)) ++c;
    return c;
}
constexpr uint64_t FIRST_WHEEL_PRIME = compute_first_wheel_prime();

// The WHEEL_SIZE residues in [1, WHEEL_MOD) coprime with WHEEL_MOD, sorted.
constexpr std::array<uint64_t, WHEEL_SIZE> make_wheel_r() {
    std::array<uint64_t, WHEEL_SIZE> r{};
    int idx = 0;
    for (uint64_t x = 1; x < WHEEL_MOD; ++x) {
        if (wheel_gcd(x, WHEEL_MOD) == 1) r[idx++] = x;
    }
    return r;
}
constexpr std::array<uint64_t, WHEEL_SIZE> WHEEL_R = make_wheel_r();

// Gap from WHEEL_R[j] to the next candidate (WHEEL_R[(j+1) % WHEEL_SIZE],
// adding WHEEL_MOD when wrapping past the end of the period).
constexpr std::array<uint64_t, WHEEL_SIZE> make_wheel_gap() {
    std::array<uint64_t, WHEEL_SIZE> gap{};
    for (int i = 0; i < WHEEL_SIZE; ++i) {
        uint64_t next = (i + 1 < WHEEL_SIZE) ? WHEEL_R[i + 1] : (WHEEL_R[0] + WHEEL_MOD);
        gap[i] = next - WHEEL_R[i];
    }
    return gap;
}
constexpr std::array<uint64_t, WHEEL_SIZE> WHEEL_GAP = make_wheel_gap();

// WHEEL_POS[r] = position of r within WHEEL_R (0..WHEEL_SIZE-1), or -1 if r
// is not coprime with WHEEL_MOD. Table generated at compile time.
constexpr std::array<int, WHEEL_MOD> make_wheel_pos() {
    std::array<int, WHEEL_MOD> pos{};
    for (auto& v : pos) v = -1;
    for (int j = 0; j < WHEEL_SIZE; ++j) pos[WHEEL_R[j]] = j;
    return pos;
}
constexpr std::array<int, WHEEL_MOD> WHEEL_POS = make_wheel_pos();

// STEP_TO_COPRIME[r] = smallest s >= 0 such that (r+s) % WHEEL_MOD is
// coprime with WHEEL_MOD. Lets callers jump straight to the next wheel
// candidate with a single lookup instead of a division-per-step search
// loop (used once per base prime per segment, not in any hot inner loop).
constexpr std::array<uint32_t, WHEEL_MOD> make_step_to_coprime() {
    std::array<uint32_t, WHEEL_MOD> step{};
    for (uint64_t r = 0; r < WHEEL_MOD; ++r) {
        uint32_t s = 0;
        while (WHEEL_POS[(r + s) % WHEEL_MOD] < 0) ++s;
        step[r] = s;
    }
    return step;
}
constexpr std::array<uint32_t, WHEEL_MOD> STEP_TO_COPRIME = make_step_to_coprime();

// The k-th number coprime with WHEEL_MOD (k=0 -> 1, k=1 -> the next one, ...).
inline uint64_t wheel_number(uint64_t k) {
    uint64_t q = k / WHEEL_SIZE;
    uint64_t j = k % WHEEL_SIZE;
    return q * WHEEL_MOD + WHEEL_R[j];
}

// Wheel index of n (n MUST be coprime with WHEEL_MOD).
inline uint64_t wheel_index(uint64_t n) {
    uint64_t q = n / WHEEL_MOD;
    uint64_t r = n % WHEEL_MOD;
    return q * WHEEL_SIZE + static_cast<uint64_t>(WHEEL_POS[r]);
}

// Counts how many numbers coprime with WHEEL_MOD are in [1, limit]
// (including 1). Equivalent to the (exclusive) wheel index of the first
// number above limit, i.e. an exclusive upper bound for k when sieving up
// to limit.
inline uint64_t wheel_count_upto(uint64_t limit) {
    uint64_t q = limit / WHEEL_MOD;
    uint64_t r = limit % WHEEL_MOD;
    uint64_t partial = 0;
    for (int j = 0; j < WHEEL_SIZE; ++j) {
        if (WHEEL_R[j] <= r) ++partial;
    }
    return q * WHEEL_SIZE + partial;
}

// A base prime (p >= FIRST_WHEEL_PRIME, i.e. not one of the wheel's own
// primes) together with its wheel-index jump table.
//
// For a multiplier m coprime with WHEEL_MOD, the product n=p*m is also
// coprime with WHEEL_MOD (gcd(p, WHEEL_MOD)=1), and every multiple of p
// that survives the wheel is of that form. delta[j] = advance of the wheel
// index of n when m moves from phase j to phase j+1 (n grows by
// p*WHEEL_GAP[j]). This advance does NOT depend on the concrete value of m,
// only on its phase j and on p, so it is computed once per prime (not per
// segment) and reused across every segment.
//
// This table is the single biggest piece of read-only state every thread
// walks through for every segment (WHEEL_SIZE entries per base prime), so
// its footprint matters for cache/memory-bandwidth behavior under many
// threads -- see the header comment. delta is stored as uint32_t (halving
// the footprint vs uint64_t): its value is bounded by roughly p *
// max(WHEEL_GAP) * WHEEL_SIZE / WHEEL_MOD, which stays well under 2^32 for
// any base prime this tool will realistically be given.
struct WheelBasePrime {
    uint64_t p;
    std::array<uint32_t, WHEEL_SIZE> delta;
};

inline std::array<uint32_t, WHEEL_SIZE> compute_wheel_deltas(uint64_t p) {
    std::array<uint32_t, WHEEL_SIZE> delta{};
    uint64_t pmod = p % WHEEL_MOD;
    for (int jj = 0; jj < WHEEL_SIZE; ++jj) {
        uint64_t rp = (pmod * WHEEL_R[jj]) % WHEEL_MOD;
        uint64_t d = p * WHEEL_GAP[jj];
        uint64_t floor_term = (rp + d) / WHEEL_MOD;
        int pos_before = WHEEL_POS[rp];
        int pos_after = WHEEL_POS[(rp + d) % WHEEL_MOD];
        // floor_term*WHEEL_SIZE always dominates (pos_after-pos_before)
        // (range -(WHEEL_SIZE-1)..(WHEEL_SIZE-1)), so the result is always >= 0.
        int64_t signed_delta = static_cast<int64_t>(floor_term) * WHEEL_SIZE
                              + (pos_after - pos_before);
        delta[jj] = static_cast<uint32_t>(signed_delta);
    }
    return delta;
}
