#ifndef HELIOS_DOCUMENT_H_
#define HELIOS_DOCUMENT_H_

#include "helios_types.h"
#include "helios_node.h"
#include "helios_parser.h"
#include <cstdint>
#include <cstddef>

namespace helios {

// Document — high-level API wrapping Parser.
// Exposes root-node access, dot-path key lookup, and node iteration.
// Also contains resolve_all_references() which contains Bug 3 (UAF).
class Document {
public:
    // Opens and parses a FBF buffer. Returns nullptr on parse failure.
    // Caller owns the returned pointer; delete with `delete`.
    static Document* open(const uint8_t* data, size_t len);

    ~Document();

    bool is_valid() const;

    const Node* root() const;
    size_t node_count() const;

    // dot-path lookup: walks map nodes matching string-table keys split by '.'
    // Returns nullptr if any segment is not found.
    const Node* find(const char* dot_path) const;

    // Expose the underlying NodePool for FieldDecoder construction in harnesses.
    const NodePool& parser_pool() const;

private:
    Document();

    // Iterates all reference nodes and materialises them into concrete copies.
    // Contains Bug 3 (UAF): calls pool.allocate() while holding a raw Node* target.
    void resolve_all_references();

    Parser parser_;
    bool   valid_;
};

}  // namespace helios
#endif  // HELIOS_DOCUMENT_H_
