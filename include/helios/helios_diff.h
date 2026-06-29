
#ifndef HELIOS_DIFF_H_
#define HELIOS_DIFF_H_

#include "helios_types.h"
#include "helios_node.h"
#include "helios_document.h"

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace helios {

// Classification of a single difference between two document trees.
enum DiffType {
    kDiffAdded,       // Node exists in right but not left
    kDiffRemoved,     // Node exists in left but not right
    kDiffModified,    // Node exists in both but value changed
    kDiffUnchanged,   // Node is identical in both trees
    kDiffMoved,       // Node exists in both but at different paths
    kDiffTypeChanged  // Same key/path, but the element type changed
};

// Returns a human-readable label for a DiffType value.
const char* diff_type_name(DiffType dt);

// One atomic difference between two trees. Each entry captures the
// JSON-pointer-style path, the kind of change, and optional string
// representations of the old and new values.
struct DiffEntry {
    std::string     path;       // e.g. "$.root.items[3].name"
    DiffType        type;
    std::string     old_value;  // textual repr, empty for Added
    std::string     new_value;  // textual repr, empty for Removed
    ElementType     old_type;   // kTypeNull when not applicable
    ElementType     new_type;   // kTypeNull when not applicable
    double          similarity; // 0.0..1.0, only set for Modified/TypeChanged

    DiffEntry()
        : type(kDiffUnchanged)
        , old_type(kTypeNull)
        , new_type(kTypeNull)
        , similarity(1.0) {}
};

// Aggregated result of a full document comparison.
struct DiffResult {
    std::vector<DiffEntry> entries;

    // Summary counters, maintained by DocumentDiffer.
    size_t added_count;
    size_t removed_count;
    size_t modified_count;
    size_t unchanged_count;
    size_t moved_count;
    size_t type_changed_count;
    size_t nodes_compared;

    DiffResult()
        : added_count(0)
        , removed_count(0)
        , modified_count(0)
        , unchanged_count(0)
        , moved_count(0)
        , type_changed_count(0)
        , nodes_compared(0) {}

    bool identical() const {
        return added_count == 0 && removed_count == 0
            && modified_count == 0 && moved_count == 0
            && type_changed_count == 0;
    }

    size_t total_changes() const {
        return added_count + removed_count + modified_count
             + moved_count + type_changed_count;
    }
};

// Knobs that control how comparison behaves.
struct DiffConfig {
    // When true, two maps are compared as unordered sets of key-value
    // pairs rather than positionally.
    bool ignore_map_order;

    // When true, arrays are compared element-by-element with move
    // detection using LCS.  When false, only positional comparison.
    bool detect_moves;

    // Floating-point tolerance: two floats are "equal" if their
    // absolute difference is less than or equal to this value.
    double float_tolerance;

    // When true, an int32 with value 42 and an int64 with value 42
    // are considered equal (type promotion).
    bool allow_type_promotion;

    // Maximum tree depth to recurse into.  0 means unlimited.
    int max_depth;

    // When true, blob contents are compared byte-by-byte.
    // When false, blobs are compared by length only.
    bool compare_blob_contents;

    // Similarity threshold (0.0..1.0).  Subtrees with similarity
    // below this are treated as Removed+Added rather than Modified.
    double similarity_threshold;

    DiffConfig()
        : ignore_map_order(true)
        , detect_moves(true)
        , float_tolerance(1e-9)
        , allow_type_promotion(true)
        , max_depth(0)
        , compare_blob_contents(true)
        , similarity_threshold(0.3) {}
};

// Main comparison engine.  Holds references to both documents for the
// duration of the compare() call; the returned DiffResult is self-
// contained and can outlive the DocumentDiffer instance.
class DocumentDiffer {
public:
    explicit DocumentDiffer(const DiffConfig& config = DiffConfig());
    ~DocumentDiffer();

    // Top-level entry point: compare two documents and return a full
    // diff result.  Both documents must be valid (is_valid() == true).
    DiffResult compare(const Document& left, const Document& right);

    // Format a human-readable summary of changes.
    static std::string format_summary(const DiffResult& result);

    // Format a patch-style output showing each change with its path.
    static std::string format_patch(const DiffResult& result);

private:
    // Internal recursive comparison of two subtrees.  Appends results
    // to result_.
    void compare_nodes(const Node* left, const Node* right,
                       const std::string& path);

    // Specialised comparators for container types.
    void compare_arrays(const Node* left, const Node* right,
                        const std::string& path);
    void compare_maps(const Node* left, const Node* right,
                      const std::string& path);

    // Compare two typed-array nodes (homogeneous element type).
    void compare_typed_arrays(const Node* left, const Node* right,
                              const std::string& path);

    // Scalar comparison; returns true when values are identical
    // (within tolerance / promotion rules).
    bool scalars_equal(const Node* a, const Node* b) const;

    // Deep equality check.
    bool nodes_equal(const Node* a, const Node* b) const;

    // Compute a 0.0..1.0 similarity score between two subtrees.
    double compute_similarity(const Node* a, const Node* b) const;

    // Add a DiffEntry to the current result and bump counters.
    void record(DiffType type, const std::string& path,
                const std::string& old_val, const std::string& new_val,
                ElementType old_type, ElementType new_type,
                double similarity = 1.0);

    // Convenience: mark an entire subtree as Added or Removed.
    void record_subtree_added(const Node* node, const std::string& path);
    void record_subtree_removed(const Node* node, const std::string& path);

    // Render a node value as a short string for display in diffs.
    std::string value_to_string(const Node* node) const;

    // Returns the key name for a node, or empty string if none.
    std::string node_key(const Node* node) const;

    // Build a child path like "$.root.items[3]" or "$.root.config.name"
    static std::string child_path(const std::string& parent,
                                  const std::string& key);
    static std::string index_path(const std::string& parent, size_t idx);

    // Helpers for type-aware numeric comparison.
    bool is_integer_type(ElementType t) const;
    bool is_float_type(ElementType t) const;
    bool is_numeric_type(ElementType t) const;
    int64_t node_as_int64(const Node* n) const;
    double node_as_double(const Node* n) const;

    // Longest common subsequence length, used for array move detection.
    size_t lcs_length(const std::vector<const Node*>& a,
                      const std::vector<const Node*>& b) const;

    DiffConfig  config_;
    DiffResult  result_;

    // Cached document pointers for key_name() lookups during comparison.
    const Document* left_doc_;
    const Document* right_doc_;
};

}  // namespace helios

#endif  // HELIOS_DIFF_H_
