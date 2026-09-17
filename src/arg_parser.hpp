#pragma once
// Parsing de argumentos de linea de comandos: tamanos con sufijos (k/m/b/t),
// hilos, rutas de salida, tamano de segmento.

#include <cstdint>
#include <string>
#include <stdexcept>
#include <cctype>
#include <cmath>
#include <thread>

struct Options {
    uint64_t limit = 0;                 // N: sieve hasta N (inclusive)
    unsigned threads = 0;                // 0 => auto (hardware_concurrency)
    std::string output = "primes.txt";   // fichero final
    uint64_t segment_width = 1u << 18;   // ancho del rango numerico por segmento (debe ser par)
    bool show_help = false;
};

// Interpreta sufijos: k=1e3 m=1e6 b=1e9 (billon ingles) t=1e12
// Tambien acepta notacion cientifica (1e11) y numeros planos (100000000000)
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
        case 'b': mult = 1e9; break;   // billon ingles (10^9)
        case 'g': mult = 1e9; break;   // giga, alias de b
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
        "Uso: %s --limit N [opciones]\n"
        "\n"
        "Criba de Eratostenes segmentada y paralela. Escribe todos los primos\n"
        "hasta N (inclusive) en un fichero de texto, uno por linea.\n"
        "\n"
        "Opciones:\n"
        "  -n, --limit N          Limite superior. Acepta sufijos k/m/b/t\n"
        "                         (b = billon ingles = 1e9). Ej: 100b = 1e11\n"
        "  -o, --output PATH      Fichero de salida (default: primes.txt)\n"
        "  -t, --threads N        Numero de hilos (default: nucleos disponibles)\n"
        "  -s, --segment-width N  Ancho numerico de cada segmento (default: 262144)\n"
        "  -h, --help             Muestra esta ayuda\n"
        "\n"
        "Ejemplos:\n"
        "  %s --limit 1000000 -o primos_1M.txt\n"
        "  %s --limit 100b -o primos_100b.txt -t 12\n",
        prog, prog, prog);
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
        } else if (a == "-n" || a == "--limit") {
            opt.limit = parse_size(need_value(i, a.c_str()));
            has_limit = true;
        } else if (a == "-o" || a == "--output") {
            opt.output = need_value(i, a.c_str());
        } else if (a == "-t" || a == "--threads") {
            opt.threads = static_cast<unsigned>(std::stoul(need_value(i, a.c_str())));
        } else if (a == "-s" || a == "--segment-width") {
            opt.segment_width = parse_size(need_value(i, a.c_str()));
        } else {
            throw std::runtime_error("argumento desconocido: " + a);
        }
    }

    if (opt.show_help) return opt;

    if (!has_limit) throw std::runtime_error("--limit es obligatorio");
    if (opt.threads == 0) {
        opt.threads = std::max(1u, std::thread::hardware_concurrency());
    }
    if (opt.segment_width < 64) opt.segment_width = 64;
    if (opt.segment_width % 2 != 0) opt.segment_width += 1; // debe ser par

    return opt;
}
