#include <cstdint>
#include <cstddef>
#include "helios/helios_varint.h"
#include "helios/helios_checksum.h"
#include "helios/helios.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < helios::kHeaderSize) return 0;

    uint64_t varint_val = 0;
    helios::decode_varint(data, size, &varint_val);
    helios::crc32_compute(data, size);

    helios::Parser parser;
    parser.parse(data, size);

    return 0;
}
