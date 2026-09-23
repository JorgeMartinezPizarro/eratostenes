// Reads a .db file produced by `eratostenes -o out.db` (SQLite, gap-encoded
// and zstd-compressed in fixed-size blocks -- see gap_block_sink.hpp and
// sqlite_prime_store.hpp for the writer side, gap_encoding.hpp for the byte
// format shared by both) and prints the Nth prime, or the total count.
//
// Lookup: find the block whose start_index is the largest one <= the
// requested (0-based) index -- idx_blocks_start makes this an indexed
// query, not a table scan -- then decompress just that one block and walk
// its gap stream up to the requested position. Metadata (blocks) and the
// compressed payload (block_data) are separate tables joined by block_id;
// see sqlite_prime_store.hpp for why (writer-side: a post-hoc start_index
// correction needs to rewrite only small metadata rows, not every block's
// compressed data too) -- from here it's just one more indexed join for
// the one matching row, no real cost.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include <sqlite3.h>
#include <zstd.h>

#include "gap_encoding.hpp"

static void print_usage(const char* prog) {
    std::fprintf(stderr,
        "Uso: %s FICHERO.db N\n"
        "     %s FICHERO.db --count\n"
        "\n"
        "Imprime el primo N-esimo (N empieza en 1: N=1 -> 2) almacenado en\n"
        "FICHERO.db, generado con 'eratostenes -o FICHERO.db'.\n"
        "\n"
        "  --count    Imprime el total de primos almacenados (pi(limit)) y sale\n"
        "  -h, --help Muestra esta ayuda\n",
        prog, prog);
}

static std::string read_meta(sqlite3* db, const char* key) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT value FROM meta WHERE key = ?;", -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error(std::string("sqlite: ") + sqlite3_errmsg(db));
    }
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
    std::string result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char* text = sqlite3_column_text(stmt, 0);
        result = text ? reinterpret_cast<const char*>(text) : "";
    } else {
        sqlite3_finalize(stmt);
        throw std::runtime_error(std::string("fichero .db invalido: falta meta.") + key);
    }
    sqlite3_finalize(stmt);
    return result;
}

// target_index is 0-based (the Nth prime, 1-indexed at the CLI, is
// target_index = N - 1 here).
static uint64_t lookup(sqlite3* db, uint64_t target_index) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT b.start_index, b.count, b.start_prime, d.data "
        "FROM blocks b JOIN block_data d ON d.block_id = b.block_id "
        "WHERE b.start_index <= ?1 ORDER BY b.start_index DESC LIMIT 1;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error(std::string("sqlite: ") + sqlite3_errmsg(db));
    }
    sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(target_index));

    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        throw std::runtime_error("indice fuera de rango: no hay ningun bloque para esa posicion");
    }

    uint64_t start_index = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
    uint64_t count = static_cast<uint64_t>(sqlite3_column_int64(stmt, 1));
    uint64_t start_prime = static_cast<uint64_t>(sqlite3_column_int64(stmt, 2));
    const void* blob = sqlite3_column_blob(stmt, 3);
    int blob_size = sqlite3_column_bytes(stmt, 3);

    if (target_index >= start_index + count) {
        sqlite3_finalize(stmt);
        throw std::runtime_error("indice fuera de rango: mas alla del ultimo primo generado");
    }

    unsigned long long raw_size = ZSTD_getFrameContentSize(blob, static_cast<size_t>(blob_size));
    if (raw_size == ZSTD_CONTENTSIZE_ERROR || raw_size == ZSTD_CONTENTSIZE_UNKNOWN) {
        sqlite3_finalize(stmt);
        throw std::runtime_error("bloque .db corrupto: tamano zstd desconocido");
    }

    std::vector<uint8_t> raw(raw_size);
    size_t decoded = ZSTD_decompress(raw.data(), raw.size(), blob, static_cast<size_t>(blob_size));
    sqlite3_finalize(stmt); // done with the blob pointer before we return
    if (ZSTD_isError(decoded) || decoded != raw_size) {
        throw std::runtime_error("bloque .db corrupto: fallo la descompresion zstd");
    }

    uint64_t value = start_prime;
    size_t pos = 0;
    uint64_t steps = target_index - start_index;
    for (uint64_t i = 0; i < steps; ++i) {
        value += decode_gap(raw.data(), pos);
    }
    return value;
}

int main(int argc, char** argv) {
    if (argc >= 2 && (std::strcmp(argv[1], "-h") == 0 || std::strcmp(argv[1], "--help") == 0)) {
        print_usage(argv[0]);
        return 0;
    }
    if (argc != 3) {
        print_usage(argv[0]);
        return 1;
    }

    const char* path = argv[1];
    const char* arg2 = argv[2];

    sqlite3* db = nullptr;
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        std::fprintf(stderr, "Error: no se pudo abrir %s: %s\n", path, sqlite3_errmsg(db));
        if (db) sqlite3_close(db);
        return 1;
    }

    try {
        if (std::strcmp(arg2, "--count") == 0) {
            std::string total = read_meta(db, "total_primes");
            std::printf("%s\n", total.c_str());
        } else {
            char* end = nullptr;
            unsigned long long n = std::strtoull(arg2, &end, 10);
            if (end == arg2 || *end != '\0' || n == 0) {
                throw std::runtime_error("N debe ser un entero >= 1");
            }
            uint64_t value = lookup(db, n - 1);
            std::printf("%llu\n", static_cast<unsigned long long>(value));
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Error: %s\n", e.what());
        sqlite3_close(db);
        return 1;
    }

    sqlite3_close(db);
    return 0;
}
