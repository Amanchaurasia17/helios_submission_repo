// src/helios_varint.cc
#include <cstdint>
#include <cstddef>
#include "helios/helios_varint.h"

namespace helios {

// Little-endian base-128 varint encoding (Protocol Buffers wire format).
// Each byte provides 7 data bits; the most-significant bit is a continuation
// flag (1 = more bytes follow, 0 = last byte).
//
// Maximum encoded width for a uint64_t is 10 bytes (70 bits, though only
// 64 are significant). We reject any input that requires more than 10 bytes
// to prevent runaway parsing on crafted input.

int decode_varint(const uint8_t* buf, size_t buf_len, uint64_t* out) {
    if (!buf || !out) {
        return -1;
    }

    uint64_t result = 0;
    int shift = 0;

    for (int i = 0; i < 10; ++i) {
        // Check that at least one byte remains.
        if (static_cast<size_t>(i) >= buf_len) {
            // Input is truncated: continuation bit was set but no byte.
            return -1;
        }

        uint8_t byte = buf[i];
        // Incorporate the 7 data bits from this byte.
        result |= static_cast<uint64_t>(byte & 0x7Fu) << shift;
        shift += 7;

        if ((byte & 0x80u) == 0) {
            // No continuation bit — this was the last byte.
            *out = result;
            return i + 1;
        }
    }

    // Reached 10 bytes and the continuation bit is still set — overlong.
    return -1;
}

int encode_varint(uint8_t* buf, size_t buf_len, uint64_t value) {
    if (!buf) {
        return -1;
    }

    int i = 0;
    do {
        if (static_cast<size_t>(i) >= buf_len) {
            // Buffer too small.
            return -1;
        }
        // Write the low 7 bits; set the continuation bit if more data remains.
        uint8_t byte = static_cast<uint8_t>(value & 0x7Fu);
        value >>= 7;
        if (value != 0) {
            byte |= 0x80u;  // more bytes follow
        }
        buf[i++] = byte;
    } while (value != 0);

    return i;
}

}  // namespace helios
