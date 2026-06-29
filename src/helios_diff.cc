
#include "helios_diff.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <utility>

namespace helios {

// ---------------------------------------------------------------------------
// Utility: human-readable name for a DiffType
// ---------------------------------------------------------------------------

const char* diff_type_name(DiffType dt) {
    switch (dt) {
        case kDiffAdded:       return "Added";
        case kDiffRemoved:     return "Removed";
        case kDiffModified:    return "Modified";
        case kDiffUnchanged:   return "Unchanged";
        case kDiffMoved:       return "Moved";
        case kDiffTypeChanged: return "TypeChanged";
    }
    return "Unknown";
}

// ---------------------------------------------------------------------------
// Utility: human-readable name for an ElementType
// ---------------------------------------------------------------------------

static const char* element_type_label(ElementType et) {
    switch (et) {
        case kTypeNull:       return "null";
        case kTypeBool:       return "bool";
        case kTypeInt8:       return "int8";
        case kTypeInt16:      return "int16";
        case kTypeInt32:      return "int32";
        case kTypeInt64:      return "int64";
        case kTypeFloat32:    return "float32";
        case kTypeFloat64:    return "float64";
        case kTypeString:     return "string";
        case kTypeBlob:       return "blob";
        case kTypeArray:      return "array";
        case kTypeMap:        return "map";
        case kTypeCompressed: return "compressed";
        case kTypeReference:  return "reference";
        case kTypeTimestamp:   return "timestamp";
        case kTypeTypedArray: return "typed_array";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// DocumentDiffer construction / destruction
// ---------------------------------------------------------------------------

DocumentDiffer::DocumentDiffer(const DiffConfig& config)
    : config_(config)
    , left_doc_(nullptr)
    , right_doc_(nullptr) {}

DocumentDiffer::~DocumentDiffer() {}

// ---------------------------------------------------------------------------
// Top-level compare
// ---------------------------------------------------------------------------

DiffResult DocumentDiffer::compare(const Document& left,
                                   const Document& right) {
    result_ = DiffResult();
    left_doc_  = &left;
    right_doc_ = &right;

    const Node* lr = left.root();
    const Node* rr = right.root();

    // Both documents empty – trivially identical.
    if (!lr && !rr) {
        return result_;
    }

    // One side missing entirely.
    if (!lr) {
        record_subtree_added(rr, "$");
        return result_;
    }
    if (!rr) {
        record_subtree_removed(lr, "$");
        return result_;
    }

    compare_nodes(lr, rr, "$");

    left_doc_  = nullptr;
    right_doc_ = nullptr;
    return result_;
}

// ---------------------------------------------------------------------------
// Recursive node comparison
// ---------------------------------------------------------------------------

void DocumentDiffer::compare_nodes(const Node* left, const Node* right,
                                   const std::string& path) {
    // Depth guard.
    if (config_.max_depth > 0) {
        // Count '.' separators as a rough depth proxy.
        int depth = 0;
        for (size_t i = 0; i < path.size(); ++i) {
            if (path[i] == '.' || path[i] == '[') ++depth;
        }
        if (depth > config_.max_depth) {
            // Beyond the configured limit – just do a shallow equality
            // check and record Modified if different.
            if (!nodes_equal(left, right)) {
                record(kDiffModified, path,
                       value_to_string(left), value_to_string(right),
                       left->type, right->type,
                       compute_similarity(left, right));
            } else {
                result_.unchanged_count++;
            }
            result_.nodes_compared++;
            return;
        }
    }

    result_.nodes_compared++;

    // If the element types differ, decide whether this qualifies as
    // a type-compatible promotion or a genuine type change.
    if (left->type != right->type) {
        // Check numeric promotion: int8->int64, float32->float64, etc.
        if (config_.allow_type_promotion
            && is_numeric_type(left->type)
            && is_numeric_type(right->type)) {
            // Both numeric – compare values despite type difference.
            if (scalars_equal(left, right)) {
                // Type differs but value is the same after promotion.
                record(kDiffTypeChanged, path,
                       value_to_string(left), value_to_string(right),
                       left->type, right->type, 1.0);
            } else {
                record(kDiffModified, path,
                       value_to_string(left), value_to_string(right),
                       left->type, right->type,
                       compute_similarity(left, right));
            }
            return;
        }

        // Types are fundamentally different (e.g. string vs array).
        double sim = compute_similarity(left, right);
        if (sim < config_.similarity_threshold) {
            // Too dissimilar – treat as remove + add.
            record_subtree_removed(left, path);
            record_subtree_added(right, path);
        } else {
            record(kDiffTypeChanged, path,
                   value_to_string(left), value_to_string(right),
                   left->type, right->type, sim);
        }
        return;
    }

    // Same element types from here on.
    switch (left->type) {
        case kTypeNull:
            // Both null – always equal.
            result_.unchanged_count++;
            break;

        case kTypeBool:
        case kTypeInt8:
        case kTypeInt16:
        case kTypeInt32:
        case kTypeInt64:
        case kTypeFloat32:
        case kTypeFloat64:
        case kTypeTimestamp:
            if (scalars_equal(left, right)) {
                result_.unchanged_count++;
            } else {
                record(kDiffModified, path,
                       value_to_string(left), value_to_string(right),
                       left->type, right->type, 0.0);
            }
            break;

        case kTypeString:
            if (left->value.str.len == right->value.str.len
                && left->value.str.data && right->value.str.data
                && memcmp(left->value.str.data, right->value.str.data,
                          left->value.str.len) == 0) {
                result_.unchanged_count++;
            } else {
                record(kDiffModified, path,
                       value_to_string(left), value_to_string(right),
                       kTypeString, kTypeString, 0.0);
            }
            break;

        case kTypeBlob: {
            bool equal = false;
            if (config_.compare_blob_contents) {
                equal = (left->value.str.len == right->value.str.len)
                     && (left->value.str.data != nullptr)
                     && (right->value.str.data != nullptr)
                     && (memcmp(left->value.str.data, right->value.str.data,
                                left->value.str.len) == 0);
            } else {
                equal = (left->value.str.len == right->value.str.len);
            }
            if (equal) {
                result_.unchanged_count++;
            } else {
                record(kDiffModified, path,
                       value_to_string(left), value_to_string(right),
                       kTypeBlob, kTypeBlob, 0.0);
            }
            break;
        }

        case kTypeArray:
            compare_arrays(left, right, path);
            break;

        case kTypeMap:
            compare_maps(left, right, path);
            break;

        case kTypeTypedArray:
            compare_typed_arrays(left, right, path);
            break;

        case kTypeCompressed:
            // Compressed nodes are opaque at this level; compare the
            // underlying data buffers byte-for-byte.
            if (left->value.str.len == right->value.str.len
                && left->value.str.data && right->value.str.data
                && memcmp(left->value.str.data, right->value.str.data,
                          left->value.str.len) == 0) {
                result_.unchanged_count++;
            } else {
                record(kDiffModified, path,
                       value_to_string(left), value_to_string(right),
                       kTypeCompressed, kTypeCompressed, 0.0);
            }
            break;

        case kTypeReference:
            if (left->value.ref_index == right->value.ref_index) {
                result_.unchanged_count++;
            } else {
                record(kDiffModified, path,
                       value_to_string(left), value_to_string(right),
                       kTypeReference, kTypeReference, 0.0);
            }
            break;
    }
}

// ---------------------------------------------------------------------------
// Array comparison
// ---------------------------------------------------------------------------

void DocumentDiffer::compare_arrays(const Node* left, const Node* right,
                                    const std::string& path) {
    if (!left_doc_ || !right_doc_) return;

    uint32_t l_base  = left->value.container.first_child_idx;
    uint32_t l_count = left->value.container.count;
    uint32_t r_base  = right->value.container.first_child_idx;
    uint32_t r_count = right->value.container.count;

    // Collect child node pointers.  We go through the document's pool
    // indirectly via the parser (exposed through Document).
    // Since Document only exposes root/find/key_name, we access the
    // pool through the parser reference stored in the Document.
    // But Document's pool isn't directly accessible, so we rely on the
    // pool embedded inside the parser.  We can get children by using
    // the node index stored in the container.

    // For move detection, build vectors of child pointers.
    std::vector<const Node*> l_children;
    std::vector<const Node*> r_children;
    l_children.reserve(l_count);
    r_children.reserve(r_count);

    for (uint32_t i = 0; i < l_count; ++i) {
        // The node pool is contiguous; children start at first_child_idx.
        const Node* child = reinterpret_cast<const Node*>(
            reinterpret_cast<const char*>(left) +
            static_cast<ptrdiff_t>((l_base + i) -
                (static_cast<uint32_t>(
                    (reinterpret_cast<const char*>(left) -
                     reinterpret_cast<const char*>(left_doc_->root()))
                    / sizeof(Node)
                ) + 0)) * 0  // avoid this brittle approach
        );
        // Actually, since node indices are absolute offsets into the
        // NodePool, and the pool is contiguous, we can compute the
        // base address by subtracting the root's index.  However, the
        // safest approach is: root is typically at index 0, so we can
        // offset from root().
        //
        // The Document stores its nodes in a flat NodePool.  root()
        // returns pool_.get(root_node_index_).  Children at index N
        // are at: root_base + (N - root_index).  We don't know the
        // root index directly, but for a well-formed document, the
        // root is the last allocated node, and children are at their
        // absolute indices.  Since root() gives us a const Node*, and
        // the pool is contiguous, we can compute any node address:
        //
        //   node_at(idx) = root() + (idx - root_idx)
        //
        // We don't have root_idx exposed, but we can reverse-engineer
        // it from node_count():  root_idx = node_count() - 1  (the
        // parser allocates root last).
        //
        // This is admittedly a hack.  The correct long-term fix is to
        // expose a pool accessor on Document, but we work with what
        // we have.

        // Compute the pool base address from root.
        size_t root_idx_l = left_doc_->node_count() - 1;
        const Node* pool_base_l = left_doc_->root() - root_idx_l;
        child = pool_base_l + (l_base + i);
        l_children.push_back(child);
    }

    for (uint32_t i = 0; i < r_count; ++i) {
        size_t root_idx_r = right_doc_->node_count() - 1;
        const Node* pool_base_r = right_doc_->root() - root_idx_r;
        const Node* child = pool_base_r + (r_base + i);
        r_children.push_back(child);
    }

    // --- Move detection with LCS ---
    if (config_.detect_moves && l_count > 0 && r_count > 0) {
        // Build an NxM equality matrix. For large arrays this is
        // O(n*m) which is acceptable for typical FBF documents
        // (hundreds, not millions of elements).

        // matched[j] = index in left that right[j] was matched to, or -1
        std::vector<int> l_matched(l_count, -1);
        std::vector<int> r_matched(r_count, -1);

        // Phase 1: LCS to find the longest sequence of matching
        //          elements in order.  We use a classic DP approach
        //          with space optimisation (two rows).
        {
            size_t n = l_count;
            size_t m = r_count;

            // dp[j] = LCS length for left[0..i-1] x right[0..j-1]
            std::vector<size_t> prev(m + 1, 0);
            std::vector<size_t> curr(m + 1, 0);

            // We also need to reconstruct, so keep a full table for
            // backtracking.  This trades memory for correctness.
            // For very large arrays (>4096), fall back to positional
            // comparison to avoid excessive memory usage.
            if (n * m > (1u << 22)) {
                // Fallback: plain positional comparison.
                size_t min_count = std::min(l_count, r_count);
                for (size_t i = 0; i < min_count; ++i) {
                    compare_nodes(l_children[i], r_children[i],
                                  index_path(path, i));
                }
                // Extra elements on the right are additions.
                for (size_t i = min_count; i < r_count; ++i) {
                    record_subtree_added(r_children[i],
                                         index_path(path, i));
                }
                // Extra elements on the left are removals.
                for (size_t i = min_count; i < l_count; ++i) {
                    record_subtree_removed(l_children[i],
                                           index_path(path, i));
                }
                return;
            }

            // Full DP table for backtracking.
            // dp_table[i][j] = LCS length of left[0..i-1], right[0..j-1]
            std::vector<std::vector<uint16_t>> dp_table(
                n + 1, std::vector<uint16_t>(m + 1, 0));

            for (size_t i = 1; i <= n; ++i) {
                for (size_t j = 1; j <= m; ++j) {
                    if (nodes_equal(l_children[i - 1], r_children[j - 1])) {
                        dp_table[i][j] = dp_table[i - 1][j - 1] + 1;
                    } else {
                        dp_table[i][j] = std::max(dp_table[i - 1][j],
                                                   dp_table[i][j - 1]);
                    }
                }
            }

            // Backtrack to find the actual LCS pairs.
            {
                size_t i = n, j = m;
                while (i > 0 && j > 0) {
                    if (nodes_equal(l_children[i - 1], r_children[j - 1])) {
                        l_matched[i - 1] = static_cast<int>(j - 1);
                        r_matched[j - 1] = static_cast<int>(i - 1);
                        --i;
                        --j;
                    } else if (dp_table[i - 1][j] >= dp_table[i][j - 1]) {
                        --i;
                    } else {
                        --j;
                    }
                }
            }
        }

        // Phase 2: for unmatched right-side elements, try to find a
        //          match in the unmatched left-side elements (moves).
        for (uint32_t j = 0; j < r_count; ++j) {
            if (r_matched[j] >= 0) continue;

            double best_sim = 0.0;
            int best_i = -1;
            for (uint32_t i = 0; i < l_count; ++i) {
                if (l_matched[i] >= 0) continue;
                double sim = compute_similarity(l_children[i], r_children[j]);
                if (sim > best_sim) {
                    best_sim = sim;
                    best_i = static_cast<int>(i);
                }
            }

            if (best_i >= 0 && best_sim >= config_.similarity_threshold) {
                l_matched[best_i] = static_cast<int>(j);
                r_matched[j] = best_i;
            }
        }

        // Phase 3: emit diff entries.
        // Matched pairs in the same position -> recurse (may be equal
        // or modified).  Matched pairs in different positions -> Moved.
        // Unmatched left -> Removed.  Unmatched right -> Added.

        for (uint32_t i = 0; i < l_count; ++i) {
            if (l_matched[i] < 0) {
                record_subtree_removed(l_children[i],
                                       index_path(path, i));
            } else {
                uint32_t j = static_cast<uint32_t>(l_matched[i]);
                if (i == j) {
                    // Same position – deep compare.
                    compare_nodes(l_children[i], r_children[j],
                                  index_path(path, i));
                } else {
                    // Different positions – record as moved, then
                    // deep compare for sub-modifications.
                    std::string lp = index_path(path, i);
                    std::string rp = index_path(path, j);
                    record(kDiffMoved, lp, lp, rp,
                           l_children[i]->type, r_children[j]->type,
                           compute_similarity(l_children[i], r_children[j]));

                    // Recurse to catch any value changes within the
                    // moved element.
                    if (!nodes_equal(l_children[i], r_children[j])) {
                        compare_nodes(l_children[i], r_children[j],
                                      index_path(path, j));
                    }
                }
            }
        }

        for (uint32_t j = 0; j < r_count; ++j) {
            if (r_matched[j] < 0) {
                record_subtree_added(r_children[j],
                                     index_path(path, j));
            }
        }
    } else {
        // No move detection – simple positional comparison.
        uint32_t min_count = std::min(l_count, r_count);
        for (uint32_t i = 0; i < min_count; ++i) {
            compare_nodes(l_children[i], r_children[i],
                          index_path(path, i));
        }
        for (uint32_t i = min_count; i < r_count; ++i) {
            record_subtree_added(r_children[i], index_path(path, i));
        }
        for (uint32_t i = min_count; i < l_count; ++i) {
            record_subtree_removed(l_children[i], index_path(path, i));
        }
    }
}

// ---------------------------------------------------------------------------
// Map comparison
// ---------------------------------------------------------------------------

void DocumentDiffer::compare_maps(const Node* left, const Node* right,
                                  const std::string& path) {
    if (!left_doc_ || !right_doc_) return;

    uint32_t l_base  = left->value.container.first_child_idx;
    uint32_t l_count = left->value.container.count;
    uint32_t r_base  = right->value.container.first_child_idx;
    uint32_t r_count = right->value.container.count;

    // Compute pool base for left and right documents.
    size_t root_idx_l = left_doc_->node_count() - 1;
    const Node* pool_base_l = left_doc_->root() - root_idx_l;

    size_t root_idx_r = right_doc_->node_count() - 1;
    const Node* pool_base_r = right_doc_->root() - root_idx_r;

    // Collect left children with their keys.
    struct KeyedNode {
        std::string key;
        const Node* node;
    };

    std::vector<KeyedNode> l_entries;
    std::vector<KeyedNode> r_entries;
    l_entries.reserve(l_count);
    r_entries.reserve(r_count);

    for (uint32_t i = 0; i < l_count; ++i) {
        const Node* child = pool_base_l + (l_base + i);
        KeyedNode kn;
        kn.node = child;

        // Try to get the key name from the document's string table.
        const char* kname = left_doc_->key_name(child);
        if (kname) {
            kn.key = kname;
        } else {
            // Fallback: use the raw string index as a synthetic key.
            char buf[32];
            snprintf(buf, sizeof(buf), "__key_%u", child->key_string_index);
            kn.key = buf;
        }
        l_entries.push_back(kn);
    }

    for (uint32_t i = 0; i < r_count; ++i) {
        const Node* child = pool_base_r + (r_base + i);
        KeyedNode kn;
        kn.node = child;

        const char* kname = right_doc_->key_name(child);
        if (kname) {
            kn.key = kname;
        } else {
            char buf[32];
            snprintf(buf, sizeof(buf), "__key_%u", child->key_string_index);
            kn.key = buf;
        }
        r_entries.push_back(kn);
    }

    if (config_.ignore_map_order) {
        // Unordered comparison: match by key name.
        // Track which right-side entries have been consumed.
        std::vector<bool> r_used(r_count, false);

        for (size_t i = 0; i < l_entries.size(); ++i) {
            const std::string& lk = l_entries[i].key;
            bool found = false;

            for (size_t j = 0; j < r_entries.size(); ++j) {
                if (r_used[j]) continue;
                if (r_entries[j].key == lk) {
                    r_used[j] = true;
                    found = true;
                    compare_nodes(l_entries[i].node, r_entries[j].node,
                                  child_path(path, lk));
                    break;
                }
            }

            if (!found) {
                // Key exists in left but not right -> Removed.
                record_subtree_removed(l_entries[i].node,
                                       child_path(path, lk));
            }
        }

        // Remaining unmatched right entries are additions.
        for (size_t j = 0; j < r_entries.size(); ++j) {
            if (r_used[j]) continue;
            record_subtree_added(r_entries[j].node,
                                 child_path(path, r_entries[j].key));
        }
    } else {
        // Ordered comparison: compare by position, but still use keys
        // for path construction.
        uint32_t min_count = std::min(l_count, r_count);
        for (uint32_t i = 0; i < min_count; ++i) {
            std::string key = l_entries[i].key;
            if (l_entries[i].key != r_entries[i].key) {
                // Different keys at same position – both old removed,
                // new added.
                record_subtree_removed(l_entries[i].node,
                                       child_path(path, l_entries[i].key));
                record_subtree_added(r_entries[i].node,
                                     child_path(path, r_entries[i].key));
            } else {
                compare_nodes(l_entries[i].node, r_entries[i].node,
                              child_path(path, key));
            }
        }
        for (uint32_t i = min_count; i < r_count; ++i) {
            record_subtree_added(r_entries[i].node,
                                 child_path(path, r_entries[i].key));
        }
        for (uint32_t i = min_count; i < l_count; ++i) {
            record_subtree_removed(l_entries[i].node,
                                   child_path(path, l_entries[i].key));
        }
    }
}

// ---------------------------------------------------------------------------
// Typed-array comparison
// ---------------------------------------------------------------------------

void DocumentDiffer::compare_typed_arrays(const Node* left, const Node* right,
                                          const std::string& path) {
    if (!left_doc_ || !right_doc_) return;

    // Typed arrays have an explicit element type tag.
    ElementType l_elem = left->value.typed_array.elem_type;
    ElementType r_elem = right->value.typed_array.elem_type;

    if (l_elem != r_elem && !config_.allow_type_promotion) {
        record(kDiffTypeChanged, path,
               element_type_label(l_elem),
               element_type_label(r_elem),
               kTypeTypedArray, kTypeTypedArray, 0.0);
        return;
    }

    uint32_t l_base  = left->value.typed_array.first_child_idx;
    uint32_t l_count = left->value.typed_array.count;
    uint32_t r_base  = right->value.typed_array.first_child_idx;
    uint32_t r_count = right->value.typed_array.count;

    size_t root_idx_l = left_doc_->node_count() - 1;
    const Node* pool_base_l = left_doc_->root() - root_idx_l;
    size_t root_idx_r = right_doc_->node_count() - 1;
    const Node* pool_base_r = right_doc_->root() - root_idx_r;

    // Simple positional comparison for typed arrays (no move detection;
    // they're typically dense numeric data).
    uint32_t min_count = std::min(l_count, r_count);
    for (uint32_t i = 0; i < min_count; ++i) {
        const Node* lc = pool_base_l + (l_base + i);
        const Node* rc = pool_base_r + (r_base + i);
        compare_nodes(lc, rc, index_path(path, i));
    }
    for (uint32_t i = min_count; i < r_count; ++i) {
        const Node* rc = pool_base_r + (r_base + i);
        record_subtree_added(rc, index_path(path, i));
    }
    for (uint32_t i = min_count; i < l_count; ++i) {
        const Node* lc = pool_base_l + (l_base + i);
        record_subtree_removed(lc, index_path(path, i));
    }
}

// ---------------------------------------------------------------------------
// Scalar equality (with tolerance and promotion)
// ---------------------------------------------------------------------------

bool DocumentDiffer::scalars_equal(const Node* a, const Node* b) const {
    if (!a || !b) return (a == b);

    // Identical types – straightforward comparison.
    if (a->type == b->type) {
        switch (a->type) {
            case kTypeBool:
                return a->value.bool_val == b->value.bool_val;
            case kTypeInt8:
                return a->value.i8_val == b->value.i8_val;
            case kTypeInt16:
                return a->value.i16_val == b->value.i16_val;
            case kTypeInt32:
                return a->value.i32_val == b->value.i32_val;
            case kTypeInt64:
                return a->value.i64_val == b->value.i64_val;
            case kTypeFloat32:
                if (config_.float_tolerance > 0.0) {
                    return fabs(static_cast<double>(a->value.f32_val) -
                                static_cast<double>(b->value.f32_val))
                           <= config_.float_tolerance;
                }
                return a->value.f32_val == b->value.f32_val;
            case kTypeFloat64:
                if (config_.float_tolerance > 0.0) {
                    return fabs(a->value.f64_val - b->value.f64_val)
                           <= config_.float_tolerance;
                }
                return a->value.f64_val == b->value.f64_val;
            case kTypeTimestamp:
                return a->value.timestamp_val == b->value.timestamp_val;
            default:
                return false;
        }
    }

    // Different types but type promotion is allowed.
    if (!config_.allow_type_promotion) return false;

    // Integer vs integer of different widths.
    if (is_integer_type(a->type) && is_integer_type(b->type)) {
        return node_as_int64(a) == node_as_int64(b);
    }

    // Float vs float of different widths.
    if (is_float_type(a->type) && is_float_type(b->type)) {
        double da = node_as_double(a);
        double db = node_as_double(b);
        if (config_.float_tolerance > 0.0) {
            return fabs(da - db) <= config_.float_tolerance;
        }
        return da == db;
    }

    // Integer vs float (cross-category promotion).
    if (is_integer_type(a->type) && is_float_type(b->type)) {
        double da = static_cast<double>(node_as_int64(a));
        double db = node_as_double(b);
        if (config_.float_tolerance > 0.0) {
            return fabs(da - db) <= config_.float_tolerance;
        }
        return da == db;
    }
    if (is_float_type(a->type) && is_integer_type(b->type)) {
        double da = node_as_double(a);
        double db = static_cast<double>(node_as_int64(b));
        if (config_.float_tolerance > 0.0) {
            return fabs(da - db) <= config_.float_tolerance;
        }
        return da == db;
    }

    // Timestamp can promote to int64 (both are 64-bit signed).
    if ((a->type == kTypeTimestamp && b->type == kTypeInt64)
        || (a->type == kTypeInt64 && b->type == kTypeTimestamp)) {
        return node_as_int64(a) == node_as_int64(b);
    }

    return false;
}

// ---------------------------------------------------------------------------
// Deep equality
// ---------------------------------------------------------------------------

bool DocumentDiffer::nodes_equal(const Node* a, const Node* b) const {
    if (!a && !b) return true;
    if (!a || !b) return false;

    // Quick reject: type mismatch (unless promotion is active and
    // both are numeric).
    if (a->type != b->type) {
        if (!config_.allow_type_promotion) return false;
        if (!is_numeric_type(a->type) || !is_numeric_type(b->type))
            return false;
        return scalars_equal(a, b);
    }

    switch (a->type) {
        case kTypeNull:
            return true;

        case kTypeBool:
        case kTypeInt8:
        case kTypeInt16:
        case kTypeInt32:
        case kTypeInt64:
        case kTypeFloat32:
        case kTypeFloat64:
        case kTypeTimestamp:
            return scalars_equal(a, b);

        case kTypeString:
            if (a->value.str.len != b->value.str.len) return false;
            if (!a->value.str.data && !b->value.str.data) return true;
            if (!a->value.str.data || !b->value.str.data) return false;
            return memcmp(a->value.str.data, b->value.str.data,
                          a->value.str.len) == 0;

        case kTypeBlob:
            if (a->value.str.len != b->value.str.len) return false;
            if (!config_.compare_blob_contents) return true;
            if (!a->value.str.data && !b->value.str.data) return true;
            if (!a->value.str.data || !b->value.str.data) return false;
            return memcmp(a->value.str.data, b->value.str.data,
                          a->value.str.len) == 0;

        case kTypeReference:
            return a->value.ref_index == b->value.ref_index;

        case kTypeArray: {
            uint32_t ac = a->value.container.count;
            uint32_t bc = b->value.container.count;
            if (ac != bc) return false;

            // We need pool access for deep child comparison.
            // This is called from contexts where left_doc_/right_doc_
            // are set.
            if (!left_doc_ || !right_doc_) return false;

            size_t ri_l = left_doc_->node_count() - 1;
            const Node* pb_l = left_doc_->root() - ri_l;
            size_t ri_r = right_doc_->node_count() - 1;
            const Node* pb_r = right_doc_->root() - ri_r;

            for (uint32_t i = 0; i < ac; ++i) {
                const Node* ca = pb_l + (a->value.container.first_child_idx + i);
                const Node* cb = pb_r + (b->value.container.first_child_idx + i);
                if (!nodes_equal(ca, cb)) return false;
            }
            return true;
        }

        case kTypeMap: {
            uint32_t ac = a->value.container.count;
            uint32_t bc = b->value.container.count;
            if (ac != bc) return false;

            if (!left_doc_ || !right_doc_) return false;

            size_t ri_l = left_doc_->node_count() - 1;
            const Node* pb_l = left_doc_->root() - ri_l;
            size_t ri_r = right_doc_->node_count() - 1;
            const Node* pb_r = right_doc_->root() - ri_r;

            if (config_.ignore_map_order) {
                // Unordered: every left child must have a matching right
                // child by key and value.
                std::vector<bool> used(bc, false);
                for (uint32_t i = 0; i < ac; ++i) {
                    const Node* la = pb_l + (a->value.container.first_child_idx + i);
                    const char* lk = left_doc_->key_name(la);
                    bool matched = false;
                    for (uint32_t j = 0; j < bc; ++j) {
                        if (used[j]) continue;
                        const Node* rb = pb_r + (b->value.container.first_child_idx + j);
                        const char* rk = right_doc_->key_name(rb);
                        if (lk && rk && strcmp(lk, rk) == 0
                            && nodes_equal(la, rb)) {
                            used[j] = true;
                            matched = true;
                            break;
                        }
                    }
                    if (!matched) return false;
                }
                return true;
            } else {
                // Ordered: positional comparison.
                for (uint32_t i = 0; i < ac; ++i) {
                    const Node* ca = pb_l + (a->value.container.first_child_idx + i);
                    const Node* cb = pb_r + (b->value.container.first_child_idx + i);
                    if (!nodes_equal(ca, cb)) return false;
                }
                return true;
            }
        }

        case kTypeTypedArray: {
            if (a->value.typed_array.elem_type != b->value.typed_array.elem_type)
                return false;
            uint32_t ac = a->value.typed_array.count;
            uint32_t bc = b->value.typed_array.count;
            if (ac != bc) return false;

            if (!left_doc_ || !right_doc_) return false;
            size_t ri_l = left_doc_->node_count() - 1;
            const Node* pb_l = left_doc_->root() - ri_l;
            size_t ri_r = right_doc_->node_count() - 1;
            const Node* pb_r = right_doc_->root() - ri_r;

            for (uint32_t i = 0; i < ac; ++i) {
                const Node* ca = pb_l + (a->value.typed_array.first_child_idx + i);
                const Node* cb = pb_r + (b->value.typed_array.first_child_idx + i);
                if (!nodes_equal(ca, cb)) return false;
            }
            return true;
        }

        case kTypeCompressed:
            if (a->value.str.len != b->value.str.len) return false;
            if (!a->value.str.data && !b->value.str.data) return true;
            if (!a->value.str.data || !b->value.str.data) return false;
            return memcmp(a->value.str.data, b->value.str.data,
                          a->value.str.len) == 0;
    }

    return false;
}

// ---------------------------------------------------------------------------
// Similarity scoring
// ---------------------------------------------------------------------------

double DocumentDiffer::compute_similarity(const Node* a, const Node* b) const {
    if (!a && !b) return 1.0;
    if (!a || !b) return 0.0;

    // Identical nodes are perfectly similar.
    if (nodes_equal(a, b)) return 1.0;

    // Different top-level types with no promotion path => low similarity.
    if (a->type != b->type) {
        if (!config_.allow_type_promotion)
            return 0.0;

        // Numeric types of different widths can still be somewhat similar.
        if (is_numeric_type(a->type) && is_numeric_type(b->type)) {
            return scalars_equal(a, b) ? 0.9 : 0.2;
        }

        // Container type mismatch: array vs map => very dissimilar.
        return 0.0;
    }

    switch (a->type) {
        case kTypeNull:
            return 1.0;

        case kTypeBool:
            return (a->value.bool_val == b->value.bool_val) ? 1.0 : 0.0;

        case kTypeInt8:
        case kTypeInt16:
        case kTypeInt32:
        case kTypeInt64:
        case kTypeTimestamp: {
            int64_t va = node_as_int64(a);
            int64_t vb = node_as_int64(b);
            if (va == vb) return 1.0;
            // For integers, define similarity as inverse of relative
            // distance, capped at a reasonable range.
            double denom = std::max(
                static_cast<double>(std::max(
                    static_cast<int64_t>(1),
                    std::max(va < 0 ? -va : va, vb < 0 ? -vb : vb))),
                1.0);
            double diff = static_cast<double>(va > vb ? va - vb : vb - va);
            double ratio = 1.0 - std::min(diff / denom, 1.0);
            return ratio;
        }

        case kTypeFloat32:
        case kTypeFloat64: {
            double da = node_as_double(a);
            double db = node_as_double(b);
            if (fabs(da - db) <= config_.float_tolerance) return 1.0;
            double denom = std::max(std::max(fabs(da), fabs(db)), 1e-15);
            double rel = fabs(da - db) / denom;
            return std::max(1.0 - rel, 0.0);
        }

        case kTypeString: {
            // String similarity: use length ratio and common-prefix
            // length as a cheap heuristic.
            size_t la = a->value.str.len;
            size_t lb = b->value.str.len;
            if (la == 0 && lb == 0) return 1.0;
            if (la == 0 || lb == 0) return 0.0;
            if (!a->value.str.data || !b->value.str.data) return 0.0;

            size_t min_len = std::min(la, lb);
            size_t max_len = std::max(la, lb);
            size_t common = 0;
            for (size_t i = 0; i < min_len; ++i) {
                if (a->value.str.data[i] == b->value.str.data[i])
                    ++common;
                else
                    break;
            }

            // Also count common suffix.
            size_t suffix_common = 0;
            for (size_t i = 0; i < min_len - common; ++i) {
                if (a->value.str.data[la - 1 - i] ==
                    b->value.str.data[lb - 1 - i])
                    ++suffix_common;
                else
                    break;
            }

            double total_common = static_cast<double>(common + suffix_common);
            return total_common / static_cast<double>(max_len);
        }

        case kTypeBlob: {
            size_t la = a->value.str.len;
            size_t lb = b->value.str.len;
            if (la == 0 && lb == 0) return 1.0;
            size_t max_len = std::max(la, lb);
            size_t min_len = std::min(la, lb);
            // Length-based similarity for blobs.
            return static_cast<double>(min_len) / static_cast<double>(max_len);
        }

        case kTypeArray: {
            uint32_t ac = a->value.container.count;
            uint32_t bc = b->value.container.count;
            if (ac == 0 && bc == 0) return 1.0;
            if (ac == 0 || bc == 0) return 0.0;

            if (!left_doc_ || !right_doc_) return 0.0;

            // Use LCS length as a similarity measure.
            size_t ri_l = left_doc_->node_count() - 1;
            const Node* pb_l = left_doc_->root() - ri_l;
            size_t ri_r = right_doc_->node_count() - 1;
            const Node* pb_r = right_doc_->root() - ri_r;

            std::vector<const Node*> va, vb;
            va.reserve(ac);
            vb.reserve(bc);
            for (uint32_t i = 0; i < ac; ++i)
                va.push_back(pb_l + (a->value.container.first_child_idx + i));
            for (uint32_t i = 0; i < bc; ++i)
                vb.push_back(pb_r + (b->value.container.first_child_idx + i));

            size_t lcs = lcs_length(va, vb);
            return (2.0 * lcs) / static_cast<double>(ac + bc);
        }

        case kTypeMap: {
            uint32_t ac = a->value.container.count;
            uint32_t bc = b->value.container.count;
            if (ac == 0 && bc == 0) return 1.0;
            if (ac == 0 || bc == 0) return 0.0;

            if (!left_doc_ || !right_doc_) return 0.0;

            size_t ri_l = left_doc_->node_count() - 1;
            const Node* pb_l = left_doc_->root() - ri_l;
            size_t ri_r = right_doc_->node_count() - 1;
            const Node* pb_r = right_doc_->root() - ri_r;

            // Count how many keys from left appear in right.
            size_t common_keys = 0;
            double value_sim_sum = 0.0;

            for (uint32_t i = 0; i < ac; ++i) {
                const Node* la = pb_l + (a->value.container.first_child_idx + i);
                const char* lk = left_doc_->key_name(la);
                if (!lk) continue;

                for (uint32_t j = 0; j < bc; ++j) {
                    const Node* rb = pb_r + (b->value.container.first_child_idx + j);
                    const char* rk = right_doc_->key_name(rb);
                    if (rk && strcmp(lk, rk) == 0) {
                        ++common_keys;
                        value_sim_sum += compute_similarity(la, rb);
                        break;
                    }
                }
            }

            // Similarity = weighted average of key overlap and value
            // similarity among common keys.
            double key_sim = (2.0 * common_keys) /
                             static_cast<double>(ac + bc);
            double val_sim = (common_keys > 0)
                           ? (value_sim_sum / common_keys)
                           : 0.0;
            return 0.5 * key_sim + 0.5 * val_sim;
        }

        case kTypeTypedArray: {
            if (a->value.typed_array.elem_type != b->value.typed_array.elem_type)
                return 0.2;  // element type mismatch penalty
            uint32_t ac = a->value.typed_array.count;
            uint32_t bc = b->value.typed_array.count;
            if (ac == 0 && bc == 0) return 1.0;
            size_t max_c = std::max(ac, bc);
            size_t min_c = std::min(ac, bc);
            return static_cast<double>(min_c) / static_cast<double>(max_c);
        }

        case kTypeCompressed:
        case kTypeReference:
            return 0.0;  // already handled above (nodes_equal returned false)
    }

    return 0.0;
}

// ---------------------------------------------------------------------------
// LCS helper for array move detection
// ---------------------------------------------------------------------------

size_t DocumentDiffer::lcs_length(const std::vector<const Node*>& a,
                                  const std::vector<const Node*>& b) const {
    size_t n = a.size();
    size_t m = b.size();

    // Bail out for very large arrays to avoid O(n*m) blowup.
    if (n * m > (1u << 22)) {
        // Approximate: count positional matches.
        size_t matches = 0;
        size_t limit = std::min(n, m);
        for (size_t i = 0; i < limit; ++i) {
            if (nodes_equal(a[i], b[i])) ++matches;
        }
        return matches;
    }

    // Space-optimised DP with two rows.
    std::vector<size_t> prev(m + 1, 0);
    std::vector<size_t> curr(m + 1, 0);

    for (size_t i = 1; i <= n; ++i) {
        curr.assign(m + 1, 0);
        for (size_t j = 1; j <= m; ++j) {
            if (nodes_equal(a[i - 1], b[j - 1])) {
                curr[j] = prev[j - 1] + 1;
            } else {
                curr[j] = std::max(prev[j], curr[j - 1]);
            }
        }
        std::swap(prev, curr);
    }

    return prev[m];
}

// ---------------------------------------------------------------------------
// Recording diffs
// ---------------------------------------------------------------------------

void DocumentDiffer::record(DiffType type, const std::string& path,
                            const std::string& old_val,
                            const std::string& new_val,
                            ElementType old_type, ElementType new_type,
                            double similarity) {
    DiffEntry entry;
    entry.path       = path;
    entry.type       = type;
    entry.old_value  = old_val;
    entry.new_value  = new_val;
    entry.old_type   = old_type;
    entry.new_type   = new_type;
    entry.similarity = similarity;

    result_.entries.push_back(entry);

    switch (type) {
        case kDiffAdded:       ++result_.added_count;        break;
        case kDiffRemoved:     ++result_.removed_count;      break;
        case kDiffModified:    ++result_.modified_count;      break;
        case kDiffUnchanged:   ++result_.unchanged_count;     break;
        case kDiffMoved:       ++result_.moved_count;         break;
        case kDiffTypeChanged: ++result_.type_changed_count;  break;
    }
}

void DocumentDiffer::record_subtree_added(const Node* node,
                                          const std::string& path) {
    if (!node) return;

    record(kDiffAdded, path, "", value_to_string(node),
           kTypeNull, node->type, 0.0);

    // Recurse into children so the full subtree is visible in the diff.
    if (node->type == kTypeArray || node->type == kTypeMap) {
        if (!right_doc_) return;
        size_t ri = right_doc_->node_count() - 1;
        const Node* pb = right_doc_->root() - ri;

        uint32_t base  = node->value.container.first_child_idx;
        uint32_t count = node->value.container.count;
        for (uint32_t i = 0; i < count; ++i) {
            const Node* child = pb + (base + i);
            std::string cp;
            if (node->type == kTypeArray) {
                cp = index_path(path, i);
            } else {
                const char* kname = right_doc_->key_name(child);
                cp = kname ? child_path(path, kname)
                           : index_path(path, i);
            }
            record_subtree_added(child, cp);
        }
    } else if (node->type == kTypeTypedArray) {
        if (!right_doc_) return;
        size_t ri = right_doc_->node_count() - 1;
        const Node* pb = right_doc_->root() - ri;

        uint32_t base  = node->value.typed_array.first_child_idx;
        uint32_t count = node->value.typed_array.count;
        for (uint32_t i = 0; i < count; ++i) {
            const Node* child = pb + (base + i);
            record_subtree_added(child, index_path(path, i));
        }
    }
}

void DocumentDiffer::record_subtree_removed(const Node* node,
                                            const std::string& path) {
    if (!node) return;

    record(kDiffRemoved, path, value_to_string(node), "",
           node->type, kTypeNull, 0.0);

    if (node->type == kTypeArray || node->type == kTypeMap) {
        if (!left_doc_) return;
        size_t ri = left_doc_->node_count() - 1;
        const Node* pb = left_doc_->root() - ri;

        uint32_t base  = node->value.container.first_child_idx;
        uint32_t count = node->value.container.count;
        for (uint32_t i = 0; i < count; ++i) {
            const Node* child = pb + (base + i);
            std::string cp;
            if (node->type == kTypeArray) {
                cp = index_path(path, i);
            } else {
                const char* kname = left_doc_->key_name(child);
                cp = kname ? child_path(path, kname)
                           : index_path(path, i);
            }
            record_subtree_removed(child, cp);
        }
    } else if (node->type == kTypeTypedArray) {
        if (!left_doc_) return;
        size_t ri = left_doc_->node_count() - 1;
        const Node* pb = left_doc_->root() - ri;

        uint32_t base  = node->value.typed_array.first_child_idx;
        uint32_t count = node->value.typed_array.count;
        for (uint32_t i = 0; i < count; ++i) {
            const Node* child = pb + (base + i);
            record_subtree_removed(child, index_path(path, i));
        }
    }
}

// ---------------------------------------------------------------------------
// Value rendering
// ---------------------------------------------------------------------------

std::string DocumentDiffer::value_to_string(const Node* node) const {
    if (!node) return "(null)";

    char buf[128];

    switch (node->type) {
        case kTypeNull:
            return "null";

        case kTypeBool:
            return node->value.bool_val ? "true" : "false";

        case kTypeInt8:
            snprintf(buf, sizeof(buf), "%d", static_cast<int>(node->value.i8_val));
            return buf;

        case kTypeInt16:
            snprintf(buf, sizeof(buf), "%d", static_cast<int>(node->value.i16_val));
            return buf;

        case kTypeInt32:
            snprintf(buf, sizeof(buf), "%d", node->value.i32_val);
            return buf;

        case kTypeInt64:
            // Use platform-appropriate format specifier.
            snprintf(buf, sizeof(buf), "%lld",
                     static_cast<long long>(node->value.i64_val));
            return buf;

        case kTypeFloat32:
            snprintf(buf, sizeof(buf), "%.7g",
                     static_cast<double>(node->value.f32_val));
            return buf;

        case kTypeFloat64:
            snprintf(buf, sizeof(buf), "%.15g", node->value.f64_val);
            return buf;

        case kTypeString:
            if (node->value.str.data && node->value.str.len > 0) {
                // Truncate very long strings in display.
                size_t show = std::min(node->value.str.len, static_cast<size_t>(64));
                std::string s(node->value.str.data, show);
                if (show < node->value.str.len) {
                    s += "...";
                }
                return "\"" + s + "\"";
            }
            return "\"\"";

        case kTypeBlob:
            snprintf(buf, sizeof(buf), "<blob:%zu bytes>",
                     node->value.str.len);
            return buf;

        case kTypeArray:
            snprintf(buf, sizeof(buf), "<array:%u elements>",
                     node->value.container.count);
            return buf;

        case kTypeMap:
            snprintf(buf, sizeof(buf), "<map:%u entries>",
                     node->value.container.count);
            return buf;

        case kTypeCompressed:
            snprintf(buf, sizeof(buf), "<compressed:%zu bytes>",
                     node->value.str.len);
            return buf;

        case kTypeReference:
            snprintf(buf, sizeof(buf), "<ref:%u>", node->value.ref_index);
            return buf;

        case kTypeTimestamp:
            snprintf(buf, sizeof(buf), "<ts:%lld>",
                     static_cast<long long>(node->value.timestamp_val));
            return buf;

        case kTypeTypedArray:
            snprintf(buf, sizeof(buf), "<typed_array[%s]:%u elements>",
                     element_type_label(node->value.typed_array.elem_type),
                     node->value.typed_array.count);
            return buf;
    }

    return "(unknown)";
}

// ---------------------------------------------------------------------------
// Node key lookup
// ---------------------------------------------------------------------------

std::string DocumentDiffer::node_key(const Node* node) const {
    if (!node) return "";

    // Try left document first, then right.
    const char* kname = nullptr;
    if (left_doc_)  kname = left_doc_->key_name(node);
    if (!kname && right_doc_) kname = right_doc_->key_name(node);
    return kname ? std::string(kname) : std::string();
}

// ---------------------------------------------------------------------------
// Path construction
// ---------------------------------------------------------------------------

std::string DocumentDiffer::child_path(const std::string& parent,
                                       const std::string& key) {
    if (parent.empty()) return key;
    return parent + "." + key;
}

std::string DocumentDiffer::index_path(const std::string& parent, size_t idx) {
    char buf[32];
    snprintf(buf, sizeof(buf), "[%zu]", idx);
    return parent + buf;
}

// ---------------------------------------------------------------------------
// Type classification helpers
// ---------------------------------------------------------------------------

bool DocumentDiffer::is_integer_type(ElementType t) const {
    return t == kTypeInt8 || t == kTypeInt16
        || t == kTypeInt32 || t == kTypeInt64;
}

bool DocumentDiffer::is_float_type(ElementType t) const {
    return t == kTypeFloat32 || t == kTypeFloat64;
}

bool DocumentDiffer::is_numeric_type(ElementType t) const {
    return is_integer_type(t) || is_float_type(t) || t == kTypeTimestamp;
}

int64_t DocumentDiffer::node_as_int64(const Node* n) const {
    if (!n) return 0;
    switch (n->type) {
        case kTypeInt8:      return static_cast<int64_t>(n->value.i8_val);
        case kTypeInt16:     return static_cast<int64_t>(n->value.i16_val);
        case kTypeInt32:     return static_cast<int64_t>(n->value.i32_val);
        case kTypeInt64:     return n->value.i64_val;
        case kTypeTimestamp: return n->value.timestamp_val;
        default:             return 0;
    }
}

double DocumentDiffer::node_as_double(const Node* n) const {
    if (!n) return 0.0;
    switch (n->type) {
        case kTypeFloat32:   return static_cast<double>(n->value.f32_val);
        case kTypeFloat64:   return n->value.f64_val;
        case kTypeInt8:      return static_cast<double>(n->value.i8_val);
        case kTypeInt16:     return static_cast<double>(n->value.i16_val);
        case kTypeInt32:     return static_cast<double>(n->value.i32_val);
        case kTypeInt64:     return static_cast<double>(n->value.i64_val);
        case kTypeTimestamp: return static_cast<double>(n->value.timestamp_val);
        default:             return 0.0;
    }
}

// ---------------------------------------------------------------------------
// Formatting: human-readable summary
// ---------------------------------------------------------------------------

std::string DocumentDiffer::format_summary(const DiffResult& result) {
    std::ostringstream out;

    out << "=== FBF Document Diff Summary ===\n";
    out << "Nodes compared:  " << result.nodes_compared << "\n";
    out << "Total changes:   " << result.total_changes() << "\n";
    out << "  Added:         " << result.added_count << "\n";
    out << "  Removed:       " << result.removed_count << "\n";
    out << "  Modified:      " << result.modified_count << "\n";
    out << "  Moved:         " << result.moved_count << "\n";
    out << "  Type changed:  " << result.type_changed_count << "\n";
    out << "  Unchanged:     " << result.unchanged_count << "\n";
    out << "Result:          "
        << (result.identical() ? "IDENTICAL" : "DIFFERENT") << "\n";

    return out.str();
}

// ---------------------------------------------------------------------------
// Formatting: patch-style output
// ---------------------------------------------------------------------------

std::string DocumentDiffer::format_patch(const DiffResult& result) {
    std::ostringstream out;

    out << "--- left\n";
    out << "+++ right\n";
    out << "@@\n";

    for (size_t i = 0; i < result.entries.size(); ++i) {
        const DiffEntry& e = result.entries[i];

        switch (e.type) {
            case kDiffAdded:
                out << "+ " << e.path;
                if (!e.new_value.empty()) {
                    out << " = " << e.new_value;
                }
                if (e.new_type != kTypeNull) {
                    out << " (" << element_type_label(e.new_type) << ")";
                }
                out << "\n";
                break;

            case kDiffRemoved:
                out << "- " << e.path;
                if (!e.old_value.empty()) {
                    out << " = " << e.old_value;
                }
                if (e.old_type != kTypeNull) {
                    out << " (" << element_type_label(e.old_type) << ")";
                }
                out << "\n";
                break;

            case kDiffModified:
                out << "~ " << e.path << "\n";
                out << "  - " << e.old_value;
                if (e.old_type != kTypeNull) {
                    out << " (" << element_type_label(e.old_type) << ")";
                }
                out << "\n";
                out << "  + " << e.new_value;
                if (e.new_type != kTypeNull) {
                    out << " (" << element_type_label(e.new_type) << ")";
                }
                out << "\n";
                break;

            case kDiffMoved:
                out << ">> " << e.old_value << " -> " << e.new_value;
                out << " (similarity: ";
                {
                    char sbuf[16];
                    snprintf(sbuf, sizeof(sbuf), "%.1f%%",
                             e.similarity * 100.0);
                    out << sbuf;
                }
                out << ")\n";
                break;

            case kDiffTypeChanged:
                out << "! " << e.path << " type: "
                    << element_type_label(e.old_type) << " -> "
                    << element_type_label(e.new_type);
                if (!e.old_value.empty() || !e.new_value.empty()) {
                    out << "  (" << e.old_value << " -> " << e.new_value << ")";
                }
                out << "\n";
                break;

            case kDiffUnchanged:
                // Normally omitted, but include if someone really
                // stored them (they won't by default since we only
                // increment the counter for unchanged nodes).
                out << "  " << e.path << " = " << e.old_value << "\n";
                break;
        }
    }

    // Footer summary.
    out << "@@\n";
    {
        char sbuf[256];
        snprintf(sbuf, sizeof(sbuf),
                 "%zu change(s): %zu added, %zu removed, %zu modified, "
                 "%zu moved, %zu type-changed",
                 result.total_changes(),
                 result.added_count,
                 result.removed_count,
                 result.modified_count,
                 result.moved_count,
                 result.type_changed_count);
        out << sbuf << "\n";
    }

    return out.str();
}

}  // namespace helios
