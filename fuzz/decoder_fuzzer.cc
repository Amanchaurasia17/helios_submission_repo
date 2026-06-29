#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include "helios/helios.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < helios::kHeaderSize) return 0;

    helios::Document* doc = helios::Document::open(data, size);
    if (!doc) return 0;

    if (doc->is_valid()) {
        const helios::Node* root = doc->root();
        if (root && root->type == helios::kTypeMap) {
            doc->find("name");
            doc->find("data.payload");
            doc->find("config.nested.deep");
        }

        // Exercise FieldDecoder on typed-array nodes (reaches Bug 1)
        helios::FieldDecoder dec(data, size, doc->parser_pool());
        for (size_t i = 0; i < doc->node_count(); i++) {
            const helios::Node* n = doc->parser_pool().get(i);
            if (!n) continue;
            if (n->type == helios::kTypeTypedArray) {
                uint8_t scratch[256];
                dec.decode_typed_array_field(n, scratch, 32);
            }
        }
    }

    delete doc;
    return 0;
}
