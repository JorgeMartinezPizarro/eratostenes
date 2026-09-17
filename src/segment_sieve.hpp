#pragma once
// Criba segmentada, solo-impares, empaquetada en bits (uint64_t words).
//
// Cada segmento cubre un rango de numeros impares [low, high) representado
// por bits: el bit i vale 1 si (low + 2*i) es COMPUESTO, 0 si es candidato
// a primo. Se usan los primos base (<= sqrt(N)) para marcar multiplos.
//
// Al extraer los primos se invierte cada palabra y se recorren los bits a 1
// (ahora "candidato a primo") con la tecnica clasica ctz + clear-lowest-bit,
// que evita comprobar bit a bit con un bucle escalar.

#include <cstdint>
#include <vector>
#include <cstring>
#include <algorithm>

class SegmentSieve {
public:
    explicit SegmentSieve(uint64_t max_odd_count) {
        size_t words = (max_odd_count + 63) / 64;
        words_.assign(words, 0);
    }

    // Criba el rango de numeros impares [low, high) (low impar, high par o
    // impar, no inclusive) usando los primos base dados (deben incluir todos
    // los primos <= sqrt(high-1); se ignora el 2, que se trata aparte).
    // odd_count = numero de impares representados = (high - low + 1) / 2
    //
    // Writer es cualquier tipo con un metodo write_uint64(uint64_t): puede
    // ser un contador (ByteCounter, sin E/S) o un escritor real (DirectWriter).
    template <typename Writer>
    void sieve_and_emit(uint64_t low, uint64_t high,
                         const std::vector<uint64_t>& base_primes,
                         Writer& out, uint64_t& prime_count) {
        uint64_t odd_count = (high > low) ? (high - low + 1) / 2 : 0;
        if (odd_count == 0) return;

        size_t words_needed = (odd_count + 63) / 64;
        std::fill(words_.begin(), words_.begin() + words_needed, 0ULL);

        for (uint64_t p : base_primes) {
            if (p < 3) continue;              // el 2 se maneja aparte
            if (p * p >= high) break;         // base_primes esta ordenado

            uint64_t start = std::max(p * p, ((low + p - 1) / p) * p);
            if ((start & 1ULL) == 0) start += p; // forzar impar (p es impar): siguiente multiplo >= low

            uint64_t step = 2 * p;
            for (uint64_t n = start; n < high; n += step) {
                uint64_t idx = (n - low) >> 1;
                words_[idx >> 6] |= (1ULL << (idx & 63));
            }
        }

        // Extraccion: bit=0 => candidato a primo. Invertimos para recorrer
        // los "1" (primos) con ctz, que es mas rapido que testear cada bit.
        for (size_t w = 0; w < words_needed; ++w) {
            uint64_t bits = ~words_[w];
            uint64_t base_idx = w * 64ULL;
            // en la ultima palabra, enmascarar los bits sobrantes mas alla de odd_count
            uint64_t remaining = odd_count - base_idx;
            if (remaining < 64) {
                bits &= (remaining == 0) ? 0ULL : ((1ULL << remaining) - 1ULL);
            }
            while (bits) {
                uint64_t bit_pos = static_cast<uint64_t>(__builtin_ctzll(bits));
                uint64_t idx = base_idx + bit_pos;
                uint64_t value = low + 2 * idx;
                out.write_uint64(value);
                ++prime_count;
                bits &= bits - 1; // limpia el bit mas bajo puesto a 1
            }
        }
    }

private:
    std::vector<uint64_t> words_;
};
