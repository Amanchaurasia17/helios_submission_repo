// include/helios/helios_node.h
#ifndef HELIOS_NODE_H_
#define HELIOS_NODE_H_

#include "helios_types.h"
#include <cstdint>
#include <cstddef>

namespace helios {

void retain_string(void* data);
void release_string(void* data);

// Node holds one parsed FBF element.
// The value union is discriminated by the `type` field.
// `key_string_index` is the string-table index of this node's map key,
// or 0xFFFFFFFF if there is no key.
// `field_offset` is the byte offset of this node's scalar data within the
// original raw input buffer (Helios-specific; used by FieldDecoder).
struct Node {
    ElementType type;
    union ValueUnion {
        bool     bool_val;
        int8_t   i8_val;
        int16_t  i16_val;
        int32_t  i32_val;
        int64_t  i64_val;
        float    f32_val;
        double   f64_val;
        int64_t  timestamp_val;
        uint32_t ref_index;
        struct { char* data; size_t len; } str;
        struct { uint32_t first_child_idx; uint32_t count; } container;
        struct {
            ElementType elem_type;
            uint32_t    first_child_idx;
            uint32_t    count;
        } typed_array;
    } value;
    uint32_t key_string_index;  // 0xFFFFFFFF = no key
    uint32_t field_offset;      // byte offset of scalar data in raw input (Helios-specific)
};

// NodePool: flat contiguous array that grows via calloc+memcpy+free.
//
// CRITICAL INVARIANT: Any raw Node* obtained from get() or allocate() is
// invalidated after any subsequent allocate() call that triggers grow().
// Callers MUST re-acquire pointers by index after any potentially-growing
// allocation. Never hold a raw Node* across an allocate() call on the same
// pool unless you are certain the pool is below capacity.
class NodePool {
public:
    NodePool();
    ~NodePool();

    // Allocates one zero-initialised Node and returns a pointer to it.
    // WARNING: may trigger grow(), which frees the old backing array and
    // invalidates ALL previously returned Node*.
    Node* allocate();

    Node*       get(size_t index);
    const Node* get(size_t index) const;

    size_t size()     const;
    size_t capacity() const;

    // Resets the pool to empty without releasing the backing allocation.
    void reset();

private:
    Node*  nodes_;
    size_t size_;
    size_t capacity_;

    // Doubles capacity using calloc + memcpy + free.
    void grow();
};

}  // namespace helios

#endif  // HELIOS_NODE_H_
