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
#include <fstream>
#include <algorithm>

#include "base_sieve.hpp"
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
        "      --l2-bytes N       Fuerza el tamano de L2 usado para el ancho\n"
        "                         de segmento automatico (default: auto-\n"
        "                         detectado via /sys; usar si la deteccion\n"
        "                         falla, p.ej. dentro de un contenedor)\n"
        "      --l1-bytes N       Fuerza el tamano de L1 (datos) usado para el\n"
        "                         sub-bloque de primos pequenos (mismo caso\n"
        "                         que --l2-bytes)\n"
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
        } else if (a == "--l2-bytes") {
            opt.l2_bytes_override = parse_size(need_value(i, a.c_str()));
        } else if (a == "--l1-bytes") {
            opt.l1_bytes_override = parse_size(need_value(i, a.c_str()));
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
        uint64_t sqrt_based = isqrt(opt.limit) * MARGIN_MULT;

        // This grows with sqrt(limit), same as the "no sparse primes at
        // all" goal above -- fine up to a point, but the per-thread bit
        // array (segment_width/WHEEL_MOD*WHEEL_SIZE bytes) grows right
        // along with it, and past some N that array stops fitting L2 (or,
        // multiplied by thread count, even L3) regardless of how good
        // "zero sparse primes" sounds -- exactly the cache-pressure
        // problem this project spent a whole prior round chasing, just
        // caused by the opposite extreme. Capping the width at a fraction
        // of the machine's actual L2 (detected, not guessed) accepts some
        // sparse primes past that point in exchange for a cache-resident
        // array -- the sparse tier's own bucket ring is pool-allocated
        // (see SegmentSieve), so that tradeoff is cheap once it's needed.
        // 1/2 of L2 leaves room for a hyperthread sibling sharing the same
        // L2 (typical topology) plus whatever else is running; falls back
        // to a conservative 256KiB if L2 can't be detected (see
        // detect_l2_cache_bytes) -- or use --l2-bytes if that fallback is
        // wrong for this machine (see the Options field comment).
        uint64_t l2_bytes = opt.l2_bytes_override ? opt.l2_bytes_override : detect_l2_cache_bytes();
        if (l2_bytes == 0) l2_bytes = 256 * 1024;
        uint64_t l2_target_bytes = l2_bytes / 2;
        // Inverse of array_bytes = segment_width * WHEEL_SIZE / WHEEL_MOD / 8
        // (see README#tuning-for-your-machine).
        uint64_t l2_based_width = l2_target_bytes * 8 * WHEEL_MOD / WHEEL_SIZE;

        opt.segment_width = std::min(sqrt_based, l2_based_width);
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
