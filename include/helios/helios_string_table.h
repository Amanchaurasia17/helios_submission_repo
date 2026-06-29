// include/helios/helios_string_table.h
#ifndef HELIOS_STRING_TABLE_H_
#define HELIOS_STRING_TABLE_H_

#include "helios_types.h"
#include <cstdint>
#include <cstddef>

namespace helios {

/**
 * StringTable — interns map-key strings from the FBF string-table section.
 *
 * The string table is loaded lazily via load() when the parser encounters the
 * kFlagHasStringTable header flag.  Loading uses a two-pass algorithm:
 *
 *   Pass 1 (scan):
 *     Iterate over all entries in the wire-format section, decoding each
 *     string-length varint without copying the string data.  Accumulate the
 *     total byte count needed for the flat string buffer, including one NUL
 *     terminator byte per entry.  Also validate each length against
 *     kMaxStringLength and reject entry_count > 100 000.
 *
 *   Allocation:
 *     Allocate entries_ array (new Entry[entry_count]) and data_buf_
 *     (malloc(total_data)) in a single step between the two passes, so only
 *     one heap allocation is needed for the string data regardless of how
 *     many entries exist.
 *
 *   Pass 2 (copy):
 *     Iterate again, this time copying each string's bytes into data_buf_ at
 *     the correct offset, appending a NUL terminator, and recording the
 *     offset and length in entries_[i].
 *
 * get_string() returns a pointer directly into data_buf_, so callers must not
 * free or modify the returned pointer.  The pointer is valid until the next
 * call to load() or reset(), or until this StringTable is destroyed.
 */
class StringTable {
public:
    StringTable();
    ~StringTable();

    /**
     * Parse and intern all strings from the raw FBF string-table section.
     * data     points to the first byte of the section (the entry_count varint).
     * data_len is the number of bytes available from that point.
     * Returns true on success, false if the section is malformed or an
     * allocation fails (the table is left in a reset/empty state on failure).
     */
    bool load(const uint8_t* data, size_t data_len);

    /**
     * Return a pointer to the NUL-terminated string at the given index.
     * If out_len is non-null, *out_len is set to the string length (excluding
     * the NUL terminator).
     * Returns nullptr if index is out of range or the table is not loaded.
     */
    const char* get_string(uint32_t index, size_t* out_len) const;

    /** True after a successful call to load(). */
    bool is_loaded() const;

    /** Release all owned memory and reset to the initial empty state. */
    void reset();

private:
    struct Entry {
        size_t offset;   ///< byte offset into data_buf_ where the string starts
        size_t length;   ///< string length in bytes, excluding the NUL terminator
    };

    Entry*   entries_;       ///< heap-allocated array of entry_count_ descriptors
    uint32_t entry_count_;   ///< number of interned strings
    char*    data_buf_;      ///< flat buffer holding all string bytes + NUL terminators
    size_t   data_buf_len_;  ///< total allocated size of data_buf_
    bool     loaded_;        ///< true iff load() completed successfully
};

}  // namespace helios

#endif  // HELIOS_STRING_TABLE_H_
