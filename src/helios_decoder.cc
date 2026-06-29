#include <cstring>
#include <cstdint>
#include <cstddef>
#include <algorithm>
#include "helios/helios_decoder.h"

namespace helios {

FieldDecoder::FieldDecoder(const uint8_t* raw_data, size_t raw_len, const NodePool& pool)
    : raw_(raw_data), raw_len_(raw_len), pool_(pool) {}

size_t FieldDecoder::scalar_size_for_type(ElementType t) const {
    switch (t) {
        case kTypeNull:      return 0;
        case kTypeBool:      return 1;
        case kTypeInt8:      return 1;
        case kTypeInt16:     return 2;
        case kTypeInt32:     return 4;
        case kTypeInt64:     return 8;
        case kTypeFloat32:   return 4;
        case kTypeFloat64:   return 8;
        case kTypeTimestamp: return 8;
        default:             return 0;
    }
}

bool FieldDecoder::decode_int32(const Node* node, int32_t* out) const {
    if (!node || !out) return false;
    if (node->type != kTypeInt32) return false;
    if (node->field_offset + 4 > raw_len_) return false;
    memcpy(out, raw_ + node->field_offset, 4);
    return true;
}

bool FieldDecoder::decode_int64(const Node* node, int64_t* out) const {
    if (!node || !out) return false;
    if (node->type != kTypeInt64) return false;
    if (node->field_offset + 8 > raw_len_) return false;
    memcpy(out, raw_ + node->field_offset, 8);
    return true;
}

bool FieldDecoder::decode_float64(const Node* node, double* out) const {
    if (!node || !out) return false;
    if (node->type != kTypeFloat64) return false;
    if (node->field_offset + 8 > raw_len_) return false;
    memcpy(out, raw_ + node->field_offset, 8);
    return true;
}

bool FieldDecoder::decode_string(const Node* node, const char** out, size_t* out_len) const {
    if (!node || !out) return false;
    if (node->type != kTypeString && node->type != kTypeBlob) return false;
    *out = node->value.str.data;
    if (out_len) *out_len = node->value.str.len;
    return true;
}

size_t FieldDecoder::decode_typed_array_field(const Node* node,
                                               void* out_buf,
                                               size_t buf_count) const {
    if (!node || !out_buf) return 0;
    if (node->type != kTypeTypedArray) return 0;

    size_t elem_size  = scalar_size_for_type(node->value.typed_array.elem_type);
    if (elem_size == 0) return 0;

    size_t copy_count = std::min(buf_count, (size_t)node->value.typed_array.count);
    size_t byte_count = copy_count * elem_size;
    if (byte_count == 0) return 0;

    const Node* first_child = pool_.get(node->value.typed_array.first_child_idx);
    if (!first_child) return 0;

    size_t src_offset = first_child->field_offset;

    // INTENTIONAL BUG (Bug 1 — OOB Read):
    // The following bounds check is INTENTIONALLY OMITTED:
    //   if (src_offset + byte_count > raw_len_) return 0;
    // This allows memcpy to read past the end of the input buffer when
    // src_offset + byte_count > raw_len_, triggering an OOB read detectable
    // by AddressSanitizer.
    memcpy(out_buf, raw_ + src_offset, byte_count);  // OOB READ when near buffer end
    return copy_count;
}

}  // namespace helios
