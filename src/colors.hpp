#pragma once
// ANSI colors for the summary output (stderr): lets times and throughput
// numbers (sieve rate, disk write rate, total primes/s) be picked out at a
// glance instead of parsed out of a wall of text. Auto-disables when stderr
// isn't a terminal (redirected to a file, piped, etc.) or when NO_COLOR is
// set, so piped/logged output stays plain; FORCE_COLOR overrides both.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <algorithm>
#include <unistd.h>

struct Colors {
    const char* reset;
    const char* bold;
    const char* dim;
    const char* label;     // field labels (conteo:, escritura:, total:)
    const char* time;      // elapsed-time values
    const char* rate;      // primes/s values
    const char* io;        // disk throughput (GB/s) values
    const char* headline;  // "Listo." line and the final total, made to stand out

    explicit Colors(bool on) {
        if (on) {
            reset    = "\033[0m";
            bold     = "\033[1m";
            dim      = "\033[2m";
            label    = "\033[36m";    // cyan
            time     = "\033[97m";    // bright white
            rate     = "\033[1;32m";  // bold green
            io       = "\033[1;35m";  // bold magenta
            headline = "\033[1;33m";  // bold yellow
        } else {
            reset = bold = dim = label = time = rate = io = headline = "";
        }
    }
};

inline bool stderr_supports_color() {
    if (std::getenv("NO_COLOR")) return false;
    const char* force = std::getenv("FORCE_COLOR");
    if (force && force[0] != '\0' && force[0] != '0') return true;
    return isatty(fileno(stderr)) != 0;
}

// Groups digits with thousands separators, e.g. 37607912018 -> "37,607,912,018".
// Large prime counts/limits are otherwise unreadable at a glance.
inline std::string format_thousands(uint64_t n) {
    std::string digits = std::to_string(n);
    std::string out;
    int since_sep = 0;
    for (auto it = digits.rbegin(); it != digits.rend(); ++it) {
        if (since_sep != 0 && since_sep % 3 == 0) out.push_back(',');
        out.push_back(*it);
        ++since_sep;
    }
    std::reverse(out.begin(), out.end());
    return out;
}
