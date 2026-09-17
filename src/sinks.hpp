#pragma once
// "Sinks" para SegmentSieve: dos formas de consumir los primos encontrados.
//
//  - ByteCounter: no escribe nada, solo cuenta cuantos bytes ocuparia el
//    resultado en texto (digitos + salto de linea). Se usa en una primera
//    pasada, sin E/S, para saber exactamente donde debe escribir cada hilo.
//
//  - DirectWriter: escribe con pwrite() en una posicion absoluta del
//    fichero final, ya pre-dimensionado. Como cada hilo tiene un rango de
//    bytes disjunto, todos pueden escribir en paralelo sobre el mismo
//    descriptor sin bloquearse ni necesitar una fusion posterior.

#include <cstdint>
#include <cstring>
#include <charconv>
#include <vector>
#include <stdexcept>
#include <unistd.h>

struct ByteCounter {
    uint64_t total_bytes = 0;

    void write_uint64(uint64_t v) {
        char scratch[24];
        auto res = std::to_chars(scratch, scratch + sizeof(scratch), v);
        total_bytes += static_cast<uint64_t>(res.ptr - scratch) + 1; // +1 por '\n'
    }
};

class DirectWriter {
public:
    DirectWriter(int fd, uint64_t start_offset, size_t buffer_size = (1u << 22)) // 4 MiB
        : fd_(fd), offset_(start_offset), buf_(buffer_size) {}

    DirectWriter(const DirectWriter&) = delete;
    DirectWriter& operator=(const DirectWriter&) = delete;

    ~DirectWriter() { try { flush(); } catch (...) {} }

    void write_uint64(uint64_t v) {
        if (pos_ + 21 > buf_.size()) flush();
        auto res = std::to_chars(buf_.data() + pos_, buf_.data() + buf_.size(), v);
        pos_ = static_cast<size_t>(res.ptr - buf_.data());
        buf_[pos_++] = '\n';
    }

    void write_raw(const char* data, size_t len) {
        if (pos_ + len > buf_.size()) flush();
        std::memcpy(buf_.data() + pos_, data, len);
        pos_ += len;
    }

    void flush() {
        size_t total_written = 0;
        while (total_written < pos_) {
            ssize_t w = ::pwrite(fd_, buf_.data() + total_written, pos_ - total_written,
                                  static_cast<off_t>(offset_ + total_written));
            if (w < 0) throw std::runtime_error("pwrite fallo escribiendo el fichero de salida");
            total_written += static_cast<size_t>(w);
        }
        offset_ += pos_;
        pos_ = 0;
    }

private:
    int fd_;
    uint64_t offset_;
    std::vector<char> buf_;
    size_t pos_ = 0;
};
