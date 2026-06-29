#include <cstdint>
#include <cstddef>
#include "helios/helios.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < helios::kHeaderSize) return 0;

    helios::Parser parser;
    parser.parse(data, size);

    // Walk every node to exercise all element types and the reference
    // resolution code path.
    size_t n = parser.node_count();
    for (size_t i = 0; i < n; i++) {
        const helios::Node* node = parser.pool().get(i);
        if (!node) continue;
        switch (node->type) {
            case helios::kTypeString:
            case helios::kTypeBlob:
                if (node->value.str.data && node->value.str.len > 0) {
                    volatile char c = node->value.str.data[0]; (void)c;
                }
                break;
            case helios::kTypeArray:
            case helios::kTypeMap: {
                volatile uint32_t idx = node->value.container.first_child_idx;
                volatile uint32_t cnt = node->value.container.count;
                (void)idx; (void)cnt;
                break;
            }
            case helios::kTypeReference: {
                volatile uint32_t ref = node->value.ref_index;
                (void)ref;
                break;
            }
            default: break;
        }
    }
    return 0;
}
