#pragma once
// Command-line argument parsing: sizes with suffixes (k/m/b/t), thread
// count, output path, segment width, count-only mode.
//
// The wheel (which primes to skip up front) is NOT here: it's a
// compile-time constant in wheel.hpp, on purpose -- see that file.

#include <cstdint>
#include <string>
#include <stdexcept>
#include <cctype>
#include <cmath>
#include <thread>

#include "base_sieve.hpp"
#include "wheel.hpp"

struct Options {
    uint64_t limit = 0;                 // N: sieve up to N (inclusive)
    unsigned threads = 0;                // 0 => auto (hardware_concurrency)
    std::string output = "primes.txt";   // final file
    // Numeric width per segment (must be even). 0 means "auto": sized from
    // N once it's known (see parse_args) so that no base prime ever
    // falls below the dense/sparse cutoff into the (bucketed, costlier)
    // sparse tier -- below that point, a *smaller* segment is strictly
    // better (its bit array fits L1/L2 more easily), so the auto default
    // is the smallest width that still keeps every base prime dense. An
    // explicit -s overrides this and is used as given, no adjustment.
    uint64_t segment_width = 0;
    bool segment_width_set = false;      // true once -s/--segment-width is parsed
    bool count_only = false;             // skip the write pass entirely
    bool show_help = false;

    // Only used when --output ends in ".db" (SQLite + zstd gap encoding,
    // see gap_block_sink.hpp / sqlite_prime_store.hpp). Ignored for plain
    // text output.
    uint64_t db_block_size = 65536;      // primes per compressed block
    int zstd_level = 3;                  // low: entropy coding (most of the
                                          // ratio, on this near-random byte
                                          // stream) barely depends on level,
                                          // so higher levels mostly buy
                                          // slower builds, not smaller files
};

// Interprets suffixes: k=1e3 m=1e6 b=1e9 (short scale billion) t=1e12
// Also accepts scientific notation (1e11) and plain numbers (100000000000)
inline uint64_t parse_size(const std::string& raw) {
    if (raw.empty()) throw std::runtime_error("valor de tamano vacio");
    std::string s = raw;
    char suffix = 0;
    char last = s.back();
    if (std::isalpha(static_cast<unsigned char>(last))) {
        suffix = static_cast<char>(std::tolower(static_cast<unsigned char>(last)));
        s.pop_back();
    }
    if (s.empty()) throw std::runtime_error("valor de tamano invalido: " + raw);

    double value;
    try {
        size_t pos = 0;
        value = std::stod(s, &pos);
        if (pos != s.size()) throw std::runtime_error("valor de tamano invalido: " + raw);
    } catch (const std::exception&) {
        throw std::runtime_error("valor de tamano invalido: " + raw);
    }

    double mult = 1.0;
    switch (suffix) {
        case 0:   mult = 1.0; break;
        case 'k': mult = 1e3; break;
        case 'm': mult = 1e6; break;
        case 'b': mult = 1e9; break;   // short scale billion (10^9)
        case 'g': mult = 1e9; break;   // giga, alias for b
        case 't': mult = 1e12; break;
        default:
            throw std::runtime_error("sufijo desconocido en tamano: " + raw);
    }
    double result = value * mult;
    if (result < 0 || result > 1.8e19) {
        throw std::runtime_error("tamano fuera de rango: " + raw);
    }
    return static_cast<uint64_t>(std::llround(result));
}

inline void print_usage(const char* prog) {
    std::fprintf(stderr,
        "Uso: %s N [opciones]\n"
        "\n"
        "Criba de Eratostenes segmentada y paralela. Escribe todos los primos\n"
        "hasta N (inclusive) en un fichero de texto, uno por linea -- o, si\n"
        "--output termina en .db, en un fichero SQLite compacto (gaps entre\n"
        "primos consecutivos, codificados a 1 byte y comprimidos con zstd por\n"
        "bloques), consultable por posicion con el binario nth_prime.\n"
        "\n"
        "Opciones:\n"
        "  N                      Limite superior. Acepta sufijos\n"
        "                         k/m/b/t (b = billon ingles = 1e9).\n"
        "                         Ej: 100b = 1e11.\n"
        "  -o, --output PATH      Fichero de salida (default: primes.txt).\n"
        "                         Si PATH termina en .db, escribe SQLite en\n"
        "                         vez de texto plano (ver arriba).\n"
        "  -t, --threads N        Numero de hilos (default: nucleos disponibles)\n"
        "  -s, --segment-width N  Ancho numerico de cada segmento\n"
        "                         (default: auto, calculado a partir de N)\n"
        "  -c, --count-only       Solo cuenta los primos, sin escribir el fichero\n"
        "      --db-block-size N  Primos por bloque comprimido en modo .db\n"
        "                         (default: 65536)\n"
        "      --zstd-level N     Nivel de compresion zstd en modo .db\n"
        "                         (default: 3)\n"
        "  -h, --help             Muestra esta ayuda\n"
        "\n"
        "La rueda (que primos se descartan de entrada) se fija en tiempo de\n"
        "compilacion en src/wheel.hpp (WHEEL_PRIMES) -- ver ese fichero para\n"
        "las configuraciones ya preparadas y por que no es un flag de CLI.\n"
        "\n"
        "Ejemplos:\n"
        "  %s 1000000 -o primos_1M.txt\n"
        "  %s 100b -o primos_100b.txt -t 12\n"
        "  %s 100b -t 12 -c\n"
        "  %s 100b -o primos_100b.db -t 12\n",
        prog, prog, prog, prog, prog);
}

inline Options parse_args(int argc, char** argv) {
    Options opt;
    auto need_value = [&](int& i, const char* name) -> std::string {
        if (i + 1 >= argc) throw std::runtime_error(std::string("falta valor para ") + name);
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
            opt.threads = static_cast<unsigned>(std::stoul(need_value(i, a.c_str())));
        } else if (a == "-s" || a == "--segment-width") {
            opt.segment_width = parse_size(need_value(i, a.c_str()));
            opt.segment_width_set = true;
        } else if (a == "-c" || a == "--count-only") {
            opt.count_only = true;
        } else if (a == "--db-block-size") {
            opt.db_block_size = parse_size(need_value(i, a.c_str()));
        } else if (a == "--zstd-level") {
            opt.zstd_level = std::stoi(need_value(i, a.c_str()));
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
            throw std::runtime_error("argumento desconocido: " + a);
        }
    }

    if (opt.show_help) return opt;

    if (!has_limit) throw std::runtime_error("falta N (limite superior)");
    if (opt.threads == 0) {
        opt.threads = std::max(1u, std::thread::hardware_concurrency());
    }
    if (!opt.segment_width_set) {
        // Smallest segment width whose k-width still covers isqrt(limit)
        // (see the field comment): WHEEL_MOD/WHEEL_SIZE converts a k-width
        // back to a numeric width, rounded up (ceiling of that ratio) as a
        // margin so integer rounding never lets a prime right at the
        // boundary slip into the sparse tier. Measured ~16% faster than a
        // fixed default at 1e12 on an i5-11400F -- see README.
        constexpr uint64_t MARGIN_MULT = (WHEEL_MOD + WHEEL_SIZE - 1) / WHEEL_SIZE;
        opt.segment_width = isqrt(opt.limit) * MARGIN_MULT;
    }
    if (opt.segment_width < 64) opt.segment_width = 64;
    if (opt.segment_width % 2 != 0) opt.segment_width += 1; // must be even

    return opt;
}
