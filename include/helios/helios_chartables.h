#ifndef HELIOS_CHARTABLES_H_
#define HELIOS_CHARTABLES_H_

// Character classification and case-mapping tables for FBF string validation.
// Generated from Unicode 15.1 data (UCD).
//
// These tables are used by the string validator to ensure that string
// elements in FBF containers are well-formed UTF-8 and to support
// case-insensitive key lookup in maps.

#include <cstdint>
#include <cstddef>

namespace helios {

// UTF-8 byte-length table: given the high nibble of the first byte,
// returns the expected number of continuation bytes (0 for ASCII).
extern const uint8_t kUtf8LenTable[256];

// Character category table: maps Unicode codepoints 0x0000-0xFFFF
// to a category enum for validation.
enum CharCategory : uint8_t {
    kCatControl     = 0,
    kCatLetter      = 1,
    kCatDigit       = 2,
    kCatPunct       = 3,
    kCatSymbol      = 4,
    kCatSeparator   = 5,
    kCatMark        = 6,
    kCatOther       = 7,
};

// Basic Multilingual Plane category table (65536 entries)
extern const uint8_t kBmpCategory[65536];

// Simple case-fold table for ASCII + Latin-1 range (256 entries)
extern const uint8_t kSimpleFold[256];

// Validate a UTF-8 string. Returns 0 on success, or the byte offset
// of the first invalid sequence.
int validate_utf8(const uint8_t* data, size_t len);

// Simple case-insensitive comparison for ASCII/Latin-1.
bool iequal_ascii(const char* a, size_t a_len, const char* b, size_t b_len);

}  // namespace helios

#endif  // HELIOS_CHARTABLES_H_
