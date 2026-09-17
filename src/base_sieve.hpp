#pragma once
// Criba simple (no segmentada) para obtener los primos base hasta sqrt(N).
// Este rango siempre es pequeno (p.ej. sqrt(1e11) ~ 316228), por lo que
// una criba de bits "solo impares" en memoria es mas que suficiente.

#include <cstdint>
#include <vector>
#include <cmath>

// Raiz cuadrada entera exacta (evita errores de redondeo de sqrt en doubles
// para numeros grandes).
inline uint64_t isqrt(uint64_t n) {
    if (n == 0) return 0;
    uint64_t s = static_cast<uint64_t>(std::sqrt(static_cast<long double>(n)));
    while (s > 0 && s * s > n) --s;
    while ((s + 1) * (s + 1) <= n) ++s;
    return s;
}

// Devuelve todos los primos <= limit.
inline std::vector<uint64_t> sieve_base_primes(uint64_t limit) {
    std::vector<uint64_t> primes;
    if (limit < 2) return primes;
    primes.push_back(2);
    if (limit < 3) return primes;

    // is_composite[i] representa al numero impar (2*i + 3)
    uint64_t count = (limit - 1) / 2; // cuantos impares >=3 hay hasta limit
    std::vector<bool> is_composite(count, false);

    for (uint64_t i = 0; i < count; ++i) {
        if (is_composite[i]) continue;
        uint64_t p = 2 * i + 3;
        if (p * p > limit) continue;
        // primer multiplo impar de p a marcar: p*p (ya es impar porque p lo es)
        for (uint64_t n = p * p; n <= limit; n += 2 * p) {
            is_composite[(n - 3) / 2] = true;
        }
    }

    for (uint64_t i = 0; i < count; ++i) {
        if (!is_composite[i]) primes.push_back(2 * i + 3);
    }
    return primes;
}
