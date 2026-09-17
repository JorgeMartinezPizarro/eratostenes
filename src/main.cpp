// Criba de Eratostenes segmentada, paralela y empaquetada en bits.
//
// Estrategia:
//   1. Se calculan los primos base (<= sqrt(N)) con una criba simple.
//   2. El rango [3, N] (impares) se divide en T fragmentos contiguos, uno
//      por hilo.
//   3. PASADA DE CONTEO: cada hilo criba su fragmento y cuenta cuantos
//      bytes de texto ocuparan sus primos (sin escribir nada a disco).
//      Con esos totales se calcula, por prefijos, el offset exacto donde
//      debe empezar a escribir cada hilo en el fichero final.
//   4. Se redimensiona el fichero final al tamano exacto ya conocido.
//   5. PASADA DE ESCRITURA: cada hilo vuelve a cribar su fragmento (mismo
//      trabajo) y esta vez escribe con pwrite() directamente en su region
//      del fichero final, en paralelo con el resto de hilos.
//
// Este diseno evita el patron "escribir en ficheros temporales + fusionar",
// que duplica la E/S en disco (se escribe el mismo byte dos veces). Aqui
// cada byte del resultado final se escribe una sola vez; el coste extra es
// repetir la fase de marcado de bits (barata, limitada por CPU/cache) en
// vez de repetir una copia de disco a disco (cara, limitada por I/O).
//
// El 2 es el unico primo par y se trata como caso especial en el hilo 0.

#include <cstdio>
#include <cstdint>
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <fcntl.h>
#include <unistd.h>

#include "arg_parser.hpp"
#include "base_sieve.hpp"
#include "segment_sieve.hpp"
#include "sinks.hpp"

namespace fs = std::filesystem;

struct ChunkRange {
    uint64_t low;   // primer impar del fragmento (inclusive)
    uint64_t high;  // limite superior (exclusivo), puede ser par
};

// Divide los impares en [3, N] en 'threads' fragmentos lo mas iguales posible.
static std::vector<ChunkRange> split_ranges(uint64_t limit, unsigned threads) {
    std::vector<ChunkRange> ranges;
    if (limit < 3) return ranges;

    uint64_t total_odds = (limit - 3) / 2 + 1; // cantidad de impares en [3, limit]
    uint64_t per_thread = (total_odds + threads - 1) / threads;
    if (per_thread == 0) per_thread = 1;

    uint64_t cursor = 3;
    uint64_t remaining = total_odds;
    for (unsigned t = 0; t < threads && remaining > 0; ++t) {
        uint64_t take = std::min(per_thread, remaining);
        uint64_t low = cursor;
        uint64_t high = low + 2 * take; // exclusivo
        ranges.push_back({low, high});
        cursor = high;
        remaining -= take;
    }
    return ranges;
}

// Pasada de conteo: no escribe nada, solo mide bytes de texto y numero de primos.
static void count_worker(ChunkRange range, uint64_t segment_width,
                          const std::vector<uint64_t>& base_primes,
                          uint64_t& out_bytes, uint64_t& out_count,
                          std::atomic<uint64_t>& progress) {
    ByteCounter counter;
    uint64_t local_count = 0;
    uint64_t max_odds_per_segment = segment_width / 2 + 1;
    SegmentSieve sieve(max_odds_per_segment);

    for (uint64_t low = range.low; low < range.high; low += segment_width) {
        uint64_t high = std::min(low + segment_width, range.high);
        sieve.sieve_and_emit(low, high, base_primes, counter, local_count);
        progress.fetch_add(high - low, std::memory_order_relaxed);
    }
    out_bytes = counter.total_bytes;
    out_count = local_count;
}

// Pasada de escritura: re-criba el mismo fragmento y escribe con pwrite()
// directamente en su region (disjunta) del fichero final.
static void emit_worker(int idx, ChunkRange range, uint64_t segment_width,
                         const std::vector<uint64_t>& base_primes,
                         int fd, uint64_t base_offset,
                         std::atomic<uint64_t>& progress) {
    DirectWriter out(fd, base_offset);
    uint64_t local_count = 0;

    if (idx == 0) {
        out.write_raw("2\n", 2);
    }

    uint64_t max_odds_per_segment = segment_width / 2 + 1;
    SegmentSieve sieve(max_odds_per_segment);

    for (uint64_t low = range.low; low < range.high; low += segment_width) {
        uint64_t high = std::min(low + segment_width, range.high);
        sieve.sieve_and_emit(low, high, base_primes, out, local_count);
        progress.fetch_add(high - low, std::memory_order_relaxed);
    }
    out.flush();
}

static void print_progress(const char* label, std::atomic<uint64_t>& progress,
                            uint64_t total, std::atomic<bool>& done) {
    using namespace std::chrono_literals;
    while (!done.load()) {
        std::this_thread::sleep_for(500ms);
        if (done.load()) break;
        uint64_t d = progress.load(std::memory_order_relaxed);
        double pct = total ? std::min(100.0, 100.0 * d / total) : 100.0;
        std::fprintf(stderr, "\r  %s: %5.1f%%   ", label, pct);
        std::fflush(stderr);
    }
    std::fprintf(stderr, "\r  %s: 100.0%%   \n", label);
}

int main(int argc, char** argv) {
    Options opt;
    try {
        opt = parse_args(argc, argv);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Error: %s\n\n", e.what());
        print_usage(argv[0]);
        return 1;
    }
    if (opt.show_help) {
        print_usage(argv[0]);
        return 0;
    }

    auto t_start = std::chrono::steady_clock::now();

    if (opt.limit < 2) {
        std::ofstream(opt.output, std::ios::binary | std::ios::trunc);
        std::fprintf(stderr, "N < 2: no hay primos. Fichero vacio creado en %s\n", opt.output.c_str());
        return 0;
    }
    if (opt.limit == 2) {
        std::ofstream ofs(opt.output, std::ios::binary | std::ios::trunc);
        ofs << "2\n";
        std::fprintf(stderr, "Listo. 1 primo escrito en %s\n", opt.output.c_str());
        return 0;
    }

    uint64_t base_limit = isqrt(opt.limit);
    std::fprintf(stderr, "Calculando primos base hasta %llu...\n",
                 static_cast<unsigned long long>(base_limit));
    std::vector<uint64_t> base_primes = sieve_base_primes(base_limit);
    std::fprintf(stderr, "  %zu primos base encontrados.\n", base_primes.size());

    auto ranges = split_ranges(opt.limit, opt.threads);
    unsigned actual_threads = static_cast<unsigned>(ranges.size());

    uint64_t total_span = ranges.back().high - ranges.front().low;

    std::fprintf(stderr, "Iniciando %u hilos, limite=%llu, segmento=%llu...\n",
                 actual_threads,
                 static_cast<unsigned long long>(opt.limit),
                 static_cast<unsigned long long>(opt.segment_width));

    // --- Pasada 1: conteo (sin E/S) ---
    std::vector<uint64_t> byte_counts(actual_threads, 0);
    std::vector<uint64_t> prime_counts(actual_threads, 0);
    {
        std::atomic<uint64_t> progress{0};
        std::atomic<bool> done{false};
        std::thread prog(print_progress, "contando", std::ref(progress), total_span, std::ref(done));

        std::vector<std::thread> pool;
        for (unsigned i = 0; i < actual_threads; ++i) {
            pool.emplace_back(count_worker, ranges[i], opt.segment_width, std::cref(base_primes),
                               std::ref(byte_counts[i]), std::ref(prime_counts[i]), std::ref(progress));
        }
        for (auto& th : pool) th.join();
        done = true;
        prog.join();
    }

    byte_counts[0] += 2; // "2\n"
    prime_counts[0] += 1;

    std::vector<uint64_t> offsets(actual_threads, 0);
    for (unsigned i = 1; i < actual_threads; ++i) offsets[i] = offsets[i - 1] + byte_counts[i - 1];
    uint64_t total_bytes = offsets.back() + byte_counts.back();
    uint64_t total_primes = 0;
    for (auto c : prime_counts) total_primes += c;

    auto t_count_done = std::chrono::steady_clock::now();

    // --- Preparar fichero final del tamano exacto ---
    {
        std::ofstream(opt.output, std::ios::binary | std::ios::trunc); // crea/trunca
    }
    fs::resize_file(opt.output, total_bytes);

    int fd = ::open(opt.output.c_str(), O_WRONLY);
    if (fd < 0) {
        std::fprintf(stderr, "Error: no se pudo abrir %s para escritura\n", opt.output.c_str());
        return 1;
    }

    // --- Pasada 2: escritura directa en paralelo ---
    {
        std::atomic<uint64_t> progress{0};
        std::atomic<bool> done{false};
        std::thread prog(print_progress, "escribiendo", std::ref(progress), total_span, std::ref(done));

        std::vector<std::thread> pool;
        for (unsigned i = 0; i < actual_threads; ++i) {
            pool.emplace_back(emit_worker, static_cast<int>(i), ranges[i], opt.segment_width,
                               std::cref(base_primes), fd, offsets[i], std::ref(progress));
        }
        for (auto& th : pool) th.join();
        done = true;
        prog.join();
    }
    ::close(fd);

    auto t_end = std::chrono::steady_clock::now();
    double count_s = std::chrono::duration<double>(t_count_done - t_start).count();
    double write_s = std::chrono::duration<double>(t_end - t_count_done).count();
    double total_s = std::chrono::duration<double>(t_end - t_start).count();

    std::fprintf(stderr,
        "Listo. %llu primos encontrados hasta %llu (%.2f GB).\n"
        "  conteo:     %.2fs\n"
        "  escritura:  %.2fs\n"
        "  total:      %.2fs (%.1f millones de primos/seg)\n",
        static_cast<unsigned long long>(total_primes),
        static_cast<unsigned long long>(opt.limit),
        total_bytes / 1e9,
        count_s, write_s, total_s,
        total_s > 0 ? (total_primes / 1e6 / total_s) : 0.0);

    return 0;
}
