// include/helios/helios_debug.h
#ifndef HELIOS_DEBUG_H_
#define HELIOS_DEBUG_H_

#include <string>
#include "helios_types.h"
#include "helios_node.h"
#include "helios_string_table.h"

namespace helios {

// ---- Helios Debugging Utilities -------------------------------------------
//
// These utilities provide recursive inspection, JSON serialization, and
// formatting for Helios Binary Format (FBF) node pools and structures.
// They are intended for developers, test harnesses, and command-line
// inspectors to visualize complex nested serialization structures.
//

/**
 * Returns a human-readable string representation of the ElementType enum value.
 *
 * @param type The ElementType enum value.
 * @return A static string naming the type, e.g., "kTypeInt32" or "kTypeMap".
 */
const char* element_type_to_string(ElementType type);

/**
 * Converts a single node's value union to a human-readable string.
 *
 * @param node Pointer to the Node to examine.
 * @param string_table The StringTable used to resolve key or string indexes.
 * @return A string representation of the node's payload value.
 */
std::string format_node_value(const Node* node, const StringTable& string_table);

/**
 * Recursively formats a Node and all of its descendants (for maps and arrays)
 * into a tree-like ASCII art string.
 *
 * Example Output:
 *  kTypeMap (root)
 *   ├── "name": kTypeString ("Helios")
 *   └── "version": kTypeInt32 (1)
 *
 * @param node Pointer to the Node to inspect (typically the root).
 * @param pool The NodePool containing the nodes.
 * @param string_table The StringTable to resolve key strings.
 * @param indent_level Current indentation level (used for recursive formatting).
 * @return A multi-line ASCII tree string.
 */
std::string dump_node_tree(const Node* node, const NodePool& pool, const StringTable& string_table, int indent_level = 0);

/**
 * Serializes a Helios Node tree to a standard JSON string.
 * This is useful for exporting structured binary documents into text formats.
 *
 * Handles nested maps, arrays, typed arrays, references, and compression wrappers.
 *
 * @param node Pointer to the Node to serialize.
 * @param pool The NodePool containing all nodes.
 * @param string_table The StringTable to resolve keys and strings.
 * @param pretty_print If true, includes indentation and newlines.
 * @param indent_level Current indentation level (used for recursive formatting).
 * @return A serialized JSON string.
 */
std::string serialize_node_to_json(const Node* node, const NodePool& pool, const StringTable& string_table, bool pretty_print = true, int indent_level = 0);

/**
 * Helper class that collects and computes statistics for a given Helios document.
 * This calculates overall node counts, type frequencies, and nesting depths.
 */
class DocumentInspector {
public:
    DocumentInspector(const Node* root, const NodePool& pool);
    
    // Compute stats
    void analyze();
    
    // Stats accessors
    size_t get_total_nodes() const;
    size_t get_type_count(ElementType type) const;
    int get_max_depth() const;
    
    // Get formatted stats report
    std::string generate_report(const StringTable& string_table) const;

private:
    void traverse(const Node* node, int current_depth);

    const Node* root_;
    const NodePool& pool_;
    
    size_t total_nodes_;
    size_t type_counts_[256];
    int max_depth_;
    bool analyzed_;
};

} // namespace helios

#endif // HELIOS_DEBUG_H_
