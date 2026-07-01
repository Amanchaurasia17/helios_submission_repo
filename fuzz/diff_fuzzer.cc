#include <cstdint>
#include <cstddef>
#include "helios/helios.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < helios::kHeaderSize) return 0;

    helios::Document* doc = helios::Document::open(data, size);
    if (!doc || !doc->is_valid()) {
        delete doc;
        return 0;
    }

    helios::DocumentDiffer differ;
    helios::DiffResult result = differ.compare(*doc, *doc);
    (void)result.total_changes();

    delete doc;
    return 0;
}
