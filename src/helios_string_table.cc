#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cstddef>
#include "helios/helios_string_table.h"
#include "helios/helios_types.h"
#include "helios/helios_varint.h"

namespace helios {

StringTable::StringTable()
    : entries_(nullptr), entry_count_(0), data_buf_(nullptr),
      data_buf_len_(0), loaded_(false) {}

StringTable::~StringTable() { reset(); }

bool StringTable::load(const uint8_t* data, size_t data_len) {
    reset();
    if (!data || data_len == 0) return false;

    size_t pos = 0;

    // Decode entry_count varint
    uint64_t ec = 0;
    int consumed = decode_varint(data + pos, data_len - pos, &ec);
    if (consumed < 0) return false;
    pos += consumed;
    if (ec > 100000) return false;
    entry_count_ = static_cast<uint32_t>(ec);
    if (entry_count_ == 0) { loaded_ = true; return true; }

    // Allocate entries array
    entries_ = new Entry[entry_count_];

    // ---- Pass 1: scan to compute total_data --------------------------------
    size_t total_data = 0;
    size_t scan_pos = pos;
    for (uint32_t i = 0; i < entry_count_; i++) {
        uint64_t slen64 = 0;
        int c = decode_varint(data + scan_pos, data_len - scan_pos, &slen64);
        if (c < 0 || slen64 > kMaxStringLength) { delete[] entries_; entries_ = nullptr; entry_count_ = 0; return false; }
        size_t slen = static_cast<size_t>(slen64);
        scan_pos += c;
        if (scan_pos + slen > data_len) { delete[] entries_; entries_ = nullptr; entry_count_ = 0; return false; }

        // INTENTIONAL BUG: last entry omits +1 for NUL terminator
        if (i + 1 < entry_count_)
            total_data += slen + 1;   // correct for all-but-last
        else
            total_data += slen;        // BUG: off-by-one on last entry (no NUL byte counted)

        scan_pos += slen;
    }

    // Allocate string buffer — one byte too short for the last string's NUL
    data_buf_ = static_cast<char*>(malloc(total_data));
    if (!data_buf_) { delete[] entries_; entries_ = nullptr; entry_count_ = 0; return false; }
    data_buf_len_ = total_data;

    // ---- Pass 2: copy strings into data_buf_ --------------------------------
    size_t doff = 0;
    for (uint32_t i = 0; i < entry_count_; i++) {
        uint64_t slen64 = 0;
        int c = decode_varint(data + pos, data_len - pos, &slen64);
        size_t slen = static_cast<size_t>(slen64);
        pos += c;
        entries_[i].offset = doff;
        entries_[i].length = slen;
        memcpy(data_buf_ + doff, data + pos, slen);
        data_buf_[doff + slen] = '\0';  // HEAP OVERFLOW on last entry: writes 1 byte past malloc boundary
        pos += slen;
        doff += slen + 1;
    }

    loaded_ = true;
    return true;
}

const char* StringTable::get_string(uint32_t index, size_t* out_len) const {
    if (!loaded_ || index >= entry_count_) return nullptr;
    if (out_len) *out_len = entries_[index].length;
    return data_buf_ + entries_[index].offset;
}

bool StringTable::is_loaded() const { return loaded_; }

void StringTable::reset() {
    if (entries_) { delete[] entries_; entries_ = nullptr; }
    if (data_buf_) { free(data_buf_); data_buf_ = nullptr; }
    entry_count_ = 0;
    data_buf_len_ = 0;
    loaded_ = false;
}

}  // namespace helios
