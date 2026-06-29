// include/helios/helios_varint.h
#ifndef HELIOS_VARINT_H_
#define HELIOS_VARINT_H_

#include <cstdint>
#include <cstddef>

namespace helios {

// Decode a little-endian base-128 (varint) encoded unsigned 64-bit integer
// from `buf` (of length `buf_len`).
//
// Returns the number of bytes consumed (>= 1) on success, or -1 on error.
// Error conditions:
//   - Truncated input (continuation bit set but no more bytes available).
//   - Overlong encoding (more than 10 bytes used).
//
// On success, the decoded value is written to *out.
int decode_varint(const uint8_t* buf, size_t buf_len, uint64_t* out);

// Encode `value` as a little-endian base-128 varint into `buf` (of size
// `buf_len`).
//
// Returns the number of bytes written (>= 1) on success, or -1 if `buf` is
// too small to hold the encoded value.
int encode_varint(uint8_t* buf, size_t buf_len, uint64_t value);

}  // namespace helios

#endif  // HELIOS_VARINT_H_
