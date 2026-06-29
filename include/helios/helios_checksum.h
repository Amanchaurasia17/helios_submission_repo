#ifndef HELIOS_CHECKSUM_H_
#define HELIOS_CHECKSUM_H_

#include <cstdint>
#include <cstddef>

namespace helios {

// Compute a standard CRC32 checksum over `len` bytes of `data`.
// Uses the ISO 3309 / zlib polynomial (0xEDB88320, reflected form).
// The 256-entry lookup table is built on first call and cached.
uint32_t crc32_compute(const uint8_t* data, size_t len);

}  // namespace helios

#endif  // HELIOS_CHECKSUM_H_
