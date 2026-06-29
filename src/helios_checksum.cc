#include <cstdint>
#include <cstddef>
#include "helios/helios_checksum.h"

namespace helios {

// File-scope lookup table and initialisation flag.
static uint32_t crc_table[256];
static bool     crc_table_ready = false;

static void build_crc_table() {
    if (crc_table_ready) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) {
            if (c & 1u)
                c = (c >> 1) ^ 0xEDB88320u;
            else
                c >>= 1;
        }
        crc_table[i] = c;
    }
    crc_table_ready = true;
}

uint32_t crc32_compute(const uint8_t* data, size_t len) {
    build_crc_table();
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc = crc_table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

}  // namespace helios
