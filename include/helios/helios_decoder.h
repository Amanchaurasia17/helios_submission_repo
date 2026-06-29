#ifndef HELIOS_DECODER_H_
#define HELIOS_DECODER_H_

#include "helios_types.h"
#include "helios_node.h"
#include <cstdint>
#include <cstddef>

namespace helios {

// FieldDecoder — typed extraction layer over an already-parsed NodePool.
// Reads scalar/typed-array field bytes directly from the original raw input
// buffer using node->field_offset, to avoid redundant allocation.
class FieldDecoder {
public:
    FieldDecoder(const uint8_t* raw_data, size_t raw_len, const NodePool& pool);

    bool decode_int32(const Node* node, int32_t* out) const;
    bool decode_int64(const Node* node, int64_t* out) const;
    bool decode_float64(const Node* node, double* out) const;
    bool decode_string(const Node* node, const char** out, size_t* out_len) const;

    // Fills out_buf with up to buf_count elements of the typed-array's element type.
    // Returns number of elements actually decoded.
    // BUG NOTE: intentionally missing bounds check before memcpy (Bug 1 — OOB Read).
    size_t decode_typed_array_field(const Node* node,
                                    void* out_buf,
                                    size_t buf_count) const;

private:
    const uint8_t* raw_;
    size_t         raw_len_;
    const NodePool& pool_;

    size_t scalar_size_for_type(ElementType t) const;
};

}  // namespace helios
#endif  // HELIOS_DECODER_H_
