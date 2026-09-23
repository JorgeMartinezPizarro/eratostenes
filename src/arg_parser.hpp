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
        // Size the segment to fill a fraction of the machine's actual,
        // detected L2 (not guessed) -- 1/2 leaves room for a hyperthread
        // sibling sharing the same L2 (typical topology) plus whatever else
        // is running; falls back to a conservative 256KiB if L2 can't be
        // detected (see detect_l2_cache_bytes), or use --l2-bytes if that
        // fallback is wrong for this machine (see the Options field
        // comment). Past this width some base primes fall into the
        // costlier sparse/bucket tier instead of staying dense -- that's
        // an accepted tradeoff, not a bug: the bucket ring is pool-
        // allocated (see SegmentSieve), so it's cheap once it's needed, far
        // cheaper than an L2-blowing segment.
        //
        // An earlier version additionally capped this at isqrt(limit) --
        // the smallest width that keeps EVERY base prime dense, avoiding
        // the sparse tier altogether below the N where that width exceeds
        // the L2 budget above. That's still correct, but it stopped being
        // the right default once the small tier's L1 sub-block (see
        // erat_small.hpp) was decoupled from segment size: before that
        // change, a smaller segment directly meant less L2 traffic for
        // every tier, so "smaller is better below the cap" made sense; the
        // small tier is now already confined to L1 regardless of segment
        // size, so shrinking the segment below the L2 budget no longer
        // helps it and only adds fixed per-segment cost (walking every
        // active prime's state, entering/exiting each tier's loop,
        // presieve fill) more often than necessary, for the medium and
        // sparse tiers that DO still scale with segment count. Measured
        // (perf stat cycles:u, i5-11400F): dropping the isqrt cap and
        // always using the L2 budget is 12.4% faster at N=1e11, 6.0%
        // faster at N=1e12 -- both regimes where isqrt(limit) used to be
        // the smaller (binding) value (isqrt gave ~41KiB/~130KiB arrays
        // there, versus the 256KiB this L2 budget allows). Past the N
        // where isqrt(limit) alone would already exceed the L2 budget
        // (roughly 1e13+ on this machine), this change is a no-op: the L2
        // budget was already the smaller, binding value even with the old
        // min(), so dropping isqrt from the comparison doesn't change the
        // result there. That's a DIFFERENT question from how big the
        // budget itself should be in that regime -- this project already
        // measured removing the /2 halving (using the full L2 instead of
        // L2/2) as a regression at N=1e13 (see this file's git history) --
        // so the /2 stays.
        uint64_t l2_bytes = opt.l2_bytes_override ? opt.l2_bytes_override : detect_l2_cache_bytes();
        if (l2_bytes == 0) l2_bytes = 256 * 1024;
        uint64_t l2_target_bytes = l2_bytes / 2;
        // Inverse of array_bytes = segment_width * WHEEL_SIZE / WHEEL_MOD / 8
        // (see README#tuning-for-your-machine).
        opt.segment_width = l2_target_bytes * 8 * WHEEL_MOD / WHEEL_SIZE;
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
