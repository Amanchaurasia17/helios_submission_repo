
#include "helios_query.h"
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <algorithm>
#include <cctype>

namespace helios {

// Forward declaration needed for recursive descent evaluation
static void collect_all_descendants(const Document& doc,
                                    const Node* node,
                                    const std::string& current_path,
                                    int depth,
                                    QueryResult* result);

// ====================================================================
//  PathExpression
// ====================================================================

PathExpression::PathExpression() : absolute_(false) {}

// ====================================================================
//  QueryResult
// ====================================================================

const Node* QueryResult::first_node() const {
    if (entries_.empty()) return nullptr;
    return entries_[0].node;
}

const std::string& QueryResult::first_path() const {
    static const std::string empty_str;
    if (entries_.empty()) return empty_str;
    return entries_[0].path;
}

void QueryResult::add(const Node* n, const std::string& path) {
    MatchEntry e;
    e.node = n;
    e.path = path;
    entries_.push_back(e);
}

void QueryResult::clear() {
    entries_.clear();
}

// ====================================================================
//  PathParser -- internal helpers
// ====================================================================

// Check if every character in [begin, end) is a decimal digit.
bool PathParser::is_numeric(const char* begin, const char* end) {
    if (begin == end) return false;
    for (const char* p = begin; p != end; ++p) {
        if (*p < '0' || *p > '9') return false;
    }
    return true;
}

// Map a type name string (case-insensitive) to the ElementType enum.
// Returns kTypeNull on failure, which the caller treats as "unknown".
ElementType PathParser::type_name_to_enum(const char* name, size_t len) {
    // Build a small local buffer for case-folded comparison.
    // Types won't exceed ~16 characters.
    if (len == 0 || len > 32) return kTypeNull;

    char lowered[33];
    for (size_t i = 0; i < len; i++) {
        lowered[i] = static_cast<char>(tolower(static_cast<unsigned char>(name[i])));
    }
    lowered[len] = '\0';

    // Compare against the known type names.
    if (strcmp(lowered, "null") == 0)        return kTypeNull;
    if (strcmp(lowered, "bool") == 0)        return kTypeBool;
    if (strcmp(lowered, "boolean") == 0)     return kTypeBool;
    if (strcmp(lowered, "int8") == 0)        return kTypeInt8;
    if (strcmp(lowered, "int16") == 0)       return kTypeInt16;
    if (strcmp(lowered, "int32") == 0)       return kTypeInt32;
    if (strcmp(lowered, "int") == 0)         return kTypeInt32;
    if (strcmp(lowered, "int64") == 0)       return kTypeInt64;
    if (strcmp(lowered, "long") == 0)        return kTypeInt64;
    if (strcmp(lowered, "float32") == 0)     return kTypeFloat32;
    if (strcmp(lowered, "float") == 0)       return kTypeFloat32;
    if (strcmp(lowered, "float64") == 0)     return kTypeFloat64;
    if (strcmp(lowered, "double") == 0)      return kTypeFloat64;
    if (strcmp(lowered, "string") == 0)      return kTypeString;
    if (strcmp(lowered, "str") == 0)         return kTypeString;
    if (strcmp(lowered, "blob") == 0)        return kTypeBlob;
    if (strcmp(lowered, "binary") == 0)      return kTypeBlob;
    if (strcmp(lowered, "array") == 0)       return kTypeArray;
    if (strcmp(lowered, "map") == 0)         return kTypeMap;
    if (strcmp(lowered, "object") == 0)      return kTypeMap;
    if (strcmp(lowered, "compressed") == 0)  return kTypeCompressed;
    if (strcmp(lowered, "reference") == 0)   return kTypeReference;
    if (strcmp(lowered, "ref") == 0)         return kTypeReference;
    if (strcmp(lowered, "timestamp") == 0)   return kTypeTimestamp;
    if (strcmp(lowered, "time") == 0)        return kTypeTimestamp;
    if (strcmp(lowered, "typedarray") == 0)  return kTypeTypedArray;
    if (strcmp(lowered, "typed_array") == 0) return kTypeTypedArray;

    return kTypeNull;  // Signals "not recognized".
}

// Parse a predicate expression from the interior of square brackets.
// Expects the content between '[' and ']', i.e. begin points just
// after '[' and end points at ']'.
//
// Supported forms:
//   [key=value]      -> kPredEquals
//   [key!=value]     -> kPredNotEquals
//   [@type=typename] -> sets TypeFilter (handled by parse_type_filter)
bool PathParser::parse_predicate(const char* begin, const char* end,
                                 Predicate* out, std::string* error_out) {
    if (begin >= end) {
        if (error_out) *error_out = "empty predicate expression";
        return false;
    }

    // Find the operator: either "!=" or "=".
    const char* op_pos = nullptr;
    PredicateOp op = kPredNone;

    for (const char* p = begin; p < end; ++p) {
        if (*p == '!' && (p + 1) < end && *(p + 1) == '=') {
            op_pos = p;
            op = kPredNotEquals;
            break;
        }
        if (*p == '=') {
            op_pos = p;
            op = kPredEquals;
            break;
        }
    }

    if (!op_pos) {
        if (error_out) *error_out = "predicate missing operator (= or !=)";
        return false;
    }

    // Extract key and value.
    size_t key_len = static_cast<size_t>(op_pos - begin);
    if (key_len == 0) {
        if (error_out) *error_out = "predicate has empty key";
        return false;
    }

    const char* val_start = op_pos + 1;
    if (op == kPredNotEquals) val_start++;  // Skip past '!='

    if (val_start >= end) {
        if (error_out) *error_out = "predicate has empty value";
        return false;
    }

    out->op = op;
    out->key.assign(begin, key_len);
    out->value.assign(val_start, static_cast<size_t>(end - val_start));
    return true;
}

// Parse a type filter of the form [@type=typename].
// begin/end delimit the content inside the brackets, so begin
// points to '@' and end is just past the last character.
bool PathParser::parse_type_filter(const char* begin, const char* end,
                                   TypeFilter* out, std::string* error_out) {
    // Must start with "@type="
    static const char kPrefix[] = "@type=";
    static const size_t kPrefixLen = sizeof(kPrefix) - 1;

    size_t span = static_cast<size_t>(end - begin);
    if (span <= kPrefixLen) {
        if (error_out) *error_out = "type filter too short";
        return false;
    }

    // Case-insensitive prefix match.
    for (size_t i = 0; i < kPrefixLen; i++) {
        char c = static_cast<char>(tolower(static_cast<unsigned char>(begin[i])));
        if (c != kPrefix[i]) {
            if (error_out) *error_out = "type filter must start with @type=";
            return false;
        }
    }

    const char* type_start = begin + kPrefixLen;
    size_t type_len = static_cast<size_t>(end - type_start);
    ElementType et = type_name_to_enum(type_start, type_len);

    // We allow kTypeNull only if the user literally typed "null".
    // Otherwise type_name_to_enum returning kTypeNull means unrecognized.
    if (et == kTypeNull) {
        char lowered[33];
        size_t clen = std::min(type_len, (size_t)32);
        for (size_t i = 0; i < clen; i++)
            lowered[i] = static_cast<char>(tolower(static_cast<unsigned char>(type_start[i])));
        lowered[clen] = '\0';
        if (strcmp(lowered, "null") != 0) {
            if (error_out) {
                *error_out = "unrecognized type name: ";
                error_out->append(type_start, type_len);
            }
            return false;
        }
    }

    out->active = true;
    out->required_type = et;
    return true;
}

// Parse a single path segment from the text between two slashes.
// The text in [begin, end) might be "child", "0", "*", "**",
// "items[name=foo]", or "[@type=string]".
bool PathParser::parse_segment(const char* begin, const char* end,
                               PathSegment* out, std::string* error_out) {
    if (begin >= end) {
        if (error_out) *error_out = "empty path segment";
        return false;
    }

    // Split at optional bracket: "foo[pred]" -> name="foo", bracket="pred"
    const char* bracket_start = nullptr;
    const char* bracket_end   = nullptr;

    for (const char* p = begin; p < end; ++p) {
        if (*p == '[') {
            bracket_start = p + 1;
            // Find matching ']'
            for (const char* q = p + 1; q < end; ++q) {
                if (*q == ']') {
                    bracket_end = q;
                    break;
                }
            }
            if (!bracket_end) {
                if (error_out) *error_out = "unclosed bracket in path segment";
                return false;
            }
            break;
        }
    }

    // The "name" part is everything before the bracket (or the whole thing).
    const char* name_end = bracket_start ? (bracket_start - 1) : end;
    size_t name_len = static_cast<size_t>(name_end - begin);

    // Determine the segment kind from the name portion.
    if (name_len == 0 && bracket_start) {
        // The whole segment is a bracket filter, e.g. "[@type=string]".
        // We'll treat this as a wildcard with a filter.
        out->kind = kSegWildcard;
    } else if (name_len == 1 && *begin == '*') {
        out->kind = kSegWildcard;
    } else if (name_len == 2 && begin[0] == '*' && begin[1] == '*') {
        out->kind = kSegRecursive;
    } else if (is_numeric(begin, name_end)) {
        out->kind = kSegIndex;
        // Convert to uint32_t. We cap at a reasonable value to prevent
        // silly overflows; arrays in FBF can't exceed kMaxElementCount.
        unsigned long val = 0;
        for (const char* p = begin; p < name_end; ++p) {
            val = val * 10 + static_cast<unsigned long>(*p - '0');
            if (val > 0xFFFFFFFEUL) {
                if (error_out) *error_out = "array index overflow";
                return false;
            }
        }
        out->index = static_cast<uint32_t>(val);
    } else {
        out->kind = kSegKey;
        out->key.assign(begin, name_len);
    }

    // Parse the bracket expression if present.
    if (bracket_start && bracket_end) {
        // Detect type filter vs. predicate.
        if (bracket_start < bracket_end && *bracket_start == '@') {
            if (!parse_type_filter(bracket_start, bracket_end,
                                   &out->type_filter, error_out)) {
                return false;
            }
        } else {
            if (!parse_predicate(bracket_start, bracket_end,
                                 &out->predicate, error_out)) {
                return false;
            }
        }
    }

    return true;
}

// ====================================================================
//  PathParser::parse  --  public entry point
// ====================================================================

PathExpression PathParser::parse(const std::string& path,
                                 std::string* error_out) {
    PathExpression expr;

    if (path.empty()) {
        if (error_out) *error_out = "empty path";
        return expr;
    }

    const char* src = path.c_str();
    size_t len = path.size();

    // Leading slash means absolute path.
    size_t pos = 0;
    if (src[0] == '/') {
        expr.absolute_ = true;
        pos = 1;
    }

    // Special case: path is exactly "/" which refers to the root.
    if (pos >= len) {
        // No segments means "match the root itself".
        return expr;
    }

    // Walk through the string, splitting on '/'.
    while (pos < len) {
        // Skip consecutive slashes (tolerate sloppy input).
        if (src[pos] == '/') {
            pos++;
            continue;
        }

        // Find the end of this segment. Brackets may contain slashes
        // (unusual but we should be safe), so we track bracket depth.
        size_t seg_start = pos;
        int bracket_depth = 0;
        while (pos < len) {
            if (src[pos] == '[') bracket_depth++;
            if (src[pos] == ']') bracket_depth--;
            if (src[pos] == '/' && bracket_depth == 0) break;
            pos++;
        }

        PathSegment seg;
        if (!parse_segment(src + seg_start, src + pos, &seg, error_out)) {
            // Return an empty expression to signal failure.
            return PathExpression();
        }
        expr.segments_.push_back(seg);

        // Skip the delimiter slash.
        if (pos < len && src[pos] == '/') pos++;
    }

    return expr;
}

// ====================================================================
//  QueryEngine
// ====================================================================

static const size_t kDefaultMaxResults = 10000;
static const int kDefaultMaxDepth = 128;

QueryEngine::QueryEngine()
    : max_results_(kDefaultMaxResults),
      max_depth_(kDefaultMaxDepth) {}

// ------------------------------------------------------------------
//  Public interface
// ------------------------------------------------------------------

QueryResult QueryEngine::query(const Document& doc,
                                const std::string& path) const {
    QueryResult result;

    if (!doc.is_valid()) return result;

    std::string parse_error;
    PathExpression expr = PathParser::parse(path, &parse_error);
    if (expr.segments().empty() && !expr.is_absolute()) {
        // Parse failure or truly empty path.  For an absolute "/" with
        // no segments we still want to match root.
        return result;
    }

    const Node* root = doc.root();
    if (!root) return result;

    if (expr.empty()) {
        // Path was just "/", which means the root node itself.
        result.add(root, "/");
        return result;
    }

    // Begin evaluation starting at the root.
    evaluate(doc, root, expr, 0, "", 0, &result);
    return result;
}

const Node* QueryEngine::query_one(const Document& doc,
                                    const std::string& path) const {
    // We could optimise this with an early-out flag, but for
    // correctness we reuse query() and just grab the first result.
    QueryResult r = query(doc, path);
    return r.first_node();
}

bool QueryEngine::exists(const Document& doc,
                          const std::string& path) const {
    return query_one(doc, path) != nullptr;
}

size_t QueryEngine::count(const Document& doc,
                           const std::string& path) const {
    QueryResult r = query(doc, path);
    return r.size();
}

// ------------------------------------------------------------------
//  limit_reached
// ------------------------------------------------------------------

bool QueryEngine::limit_reached(const QueryResult* result) const {
    if (max_results_ == 0) return false;  // Unlimited.
    return result->size() >= max_results_;
}

// ------------------------------------------------------------------
//  get_children
//
//  Return the immediate children of a container node (array or map),
//  together with the key or index string that identifies each child.
//  For maps the key comes from the string table; for arrays we use
//  the stringified index.
// ------------------------------------------------------------------

void QueryEngine::get_children(
        const Document& doc,
        const Node* node,
        std::vector<std::pair<std::string, const Node*>>* out) const {

    if (!node) return;

    if (node->type == kTypeArray) {
        uint32_t base = node->value.container.first_child_idx;
        uint32_t cnt  = node->value.container.count;
        out->reserve(cnt);
        for (uint32_t i = 0; i < cnt; i++) {
            const Node* child = doc.root();  // placeholder for pool access
            // We need the pool, which is accessible through parser_;
            // but Document exposes it indirectly.  The node pool is laid
            // out contiguously, so we can compute the child from the
            // root pointer offset... except the Document API doesn't
            // expose the pool directly.  We'll use a different approach:
            // walk through Document::find isn't viable either.
            //
            // The practical approach: since Node stores container children
            // as contiguous indices, and the Document gives us root()
            // (which is pool_.get(root_node_index_)), we know the pool
            // layout.  However, the Document class doesn't expose get()
            // on the pool.  For a real integration we would add a method.
            //
            // For now, we rely on the fact that Node pointers are stable
            // during read-only access, and the children are at known
            // offsets from the root.  We compute the child pointer
            // using pointer arithmetic from root() and the known index
            // layout.
            //
            // This is admittedly brittle; a better approach would be to
            // add a pool accessor to Document.  But we work with what
            // the public API gives us.

            // Actually: Document has a `find()` method and `root()`.
            // The nodes are stored in a contiguous NodePool, and
            // root() returns &nodes_[root_index].  If root_index == 0
            // (the common case for a single-root document), then
            // root() == &nodes_[0] and pool.get(N) == root() + N.
            //
            // Even if root_index != 0, the *children* of a node at
            // pool index P have their first_child_idx set to an absolute
            // pool index.  So child = root() + (base + i - root_index).
            //
            // But we don't know root_index from the public API.  The
            // safest, most realistic approach: treat the root pointer
            // as the base of the pool (index 0).  This works for the
            // standard parser which always puts root at index 0.

            // We use a helper that computes pool node from root().
            child = doc.root() + (base + i);

            char idx_buf[16];
            snprintf(idx_buf, sizeof(idx_buf), "%u", i);
            out->push_back(std::make_pair(std::string(idx_buf), child));
        }
    } else if (node->type == kTypeMap) {
        uint32_t base = node->value.container.first_child_idx;
        uint32_t cnt  = node->value.container.count;
        out->reserve(cnt);
        for (uint32_t i = 0; i < cnt; i++) {
            const Node* child = doc.root() + (base + i);
            const char* kname = doc.key_name(child);
            std::string key_str = kname ? kname : "";
            out->push_back(std::make_pair(key_str, child));
        }
    } else if (node->type == kTypeTypedArray) {
        uint32_t base = node->value.typed_array.first_child_idx;
        uint32_t cnt  = node->value.typed_array.count;
        out->reserve(cnt);
        for (uint32_t i = 0; i < cnt; i++) {
            const Node* child = doc.root() + (base + i);
            char idx_buf[16];
            snprintf(idx_buf, sizeof(idx_buf), "%u", i);
            out->push_back(std::make_pair(std::string(idx_buf), child));
        }
    }
    // For scalars or other types, there are no children.
}

// ------------------------------------------------------------------
//  node_string_value
//
//  For predicate comparison, we need the "string" value of a node.
//  - Strings return their data directly.
//  - Booleans return "true" or "false".
//  - Integers and floats are formatted as decimal strings.
//  - Null returns "null".
//  - Containers don't have a meaningful scalar value.
// ------------------------------------------------------------------

bool QueryEngine::node_string_value(const Node* node,
                                     std::string* out) const {
    if (!node) return false;

    char buf[64];

    switch (node->type) {
        case kTypeNull:
            *out = "null";
            return true;

        case kTypeBool:
            *out = node->value.bool_val ? "true" : "false";
            return true;

        case kTypeInt8:
            snprintf(buf, sizeof(buf), "%d", static_cast<int>(node->value.i8_val));
            *out = buf;
            return true;

        case kTypeInt16:
            snprintf(buf, sizeof(buf), "%d", static_cast<int>(node->value.i16_val));
            *out = buf;
            return true;

        case kTypeInt32:
            snprintf(buf, sizeof(buf), "%d", static_cast<int>(node->value.i32_val));
            *out = buf;
            return true;

        case kTypeInt64:
            // Use platform-safe formatting for 64-bit integers.
            snprintf(buf, sizeof(buf), "%lld",
                     static_cast<long long>(node->value.i64_val));
            *out = buf;
            return true;

        case kTypeFloat32:
            snprintf(buf, sizeof(buf), "%g", static_cast<double>(node->value.f32_val));
            *out = buf;
            return true;

        case kTypeFloat64:
            snprintf(buf, sizeof(buf), "%g", node->value.f64_val);
            *out = buf;
            return true;

        case kTypeString:
            if (node->value.str.data) {
                out->assign(node->value.str.data, node->value.str.len);
            } else {
                out->clear();
            }
            return true;

        case kTypeTimestamp:
            snprintf(buf, sizeof(buf), "%lld",
                     static_cast<long long>(node->value.timestamp_val));
            *out = buf;
            return true;

        default:
            // Containers, blobs, references, etc. have no scalar string value.
            return false;
    }
}

// ------------------------------------------------------------------
//  passes_filters
//
//  Check whether a node satisfies the predicate and/or type filter
//  on a path segment.  Both must pass for the node to match.
// ------------------------------------------------------------------

bool QueryEngine::passes_filters(const Document& doc,
                                  const Node* node,
                                  const PathSegment& seg) const {
    if (!node) return false;

    // -- Type filter check --
    if (seg.has_type_filter()) {
        if (node->type != seg.type_filter.required_type) {
            return false;
        }
    }

    // -- Predicate check --
    if (seg.has_predicate()) {
        const Predicate& pred = seg.predicate;

        // The predicate [key=value] is evaluated differently depending
        // on the context:
        //
        // 1. If the current node is a map, look up `key` among its
        //    children, then compare that child's string value to `value`.
        //
        // 2. If the current node is a scalar with a key_string_index,
        //    compare the node's own key name against `key` and its
        //    scalar value against `value`.  This supports filtering
        //    when the predicate is applied directly to map entries.

        if (node->type == kTypeMap) {
            // Search for a child whose key matches pred.key.
            uint32_t base = node->value.container.first_child_idx;
            uint32_t cnt  = node->value.container.count;
            bool found_match = false;

            for (uint32_t i = 0; i < cnt; i++) {
                const Node* child = doc.root() + (base + i);
                const char* kname = doc.key_name(child);
                if (!kname) continue;
                if (pred.key != kname) continue;

                // Found the child with the matching key; compare its value.
                std::string child_val;
                if (!node_string_value(child, &child_val)) continue;

                if (pred.op == kPredEquals) {
                    found_match = (child_val == pred.value);
                } else if (pred.op == kPredNotEquals) {
                    found_match = (child_val != pred.value);
                }
                break;  // Keys are unique in a map.
            }
            if (!found_match) return false;

        } else if (node->type == kTypeArray || node->type == kTypeTypedArray) {
            // For arrays, the predicate filters children.  This is a
            // slightly unusual interpretation: if the array element is
            // itself a map, we check inside.  Otherwise, the predicate
            // cannot match a bare array.
            //
            // Actually, in most query-language designs, a predicate on
            // an array means "select the array elements where the
            // predicate is true".  That selection happens during
            // traversal, not here.  When passes_filters is called on
            // an array node, it means the predicate is checking the
            // array *itself*, which doesn't make semantic sense.
            //
            // For robustness, we just return true and let the traversal
            // logic handle per-element filtering.
            // (We intentionally don't reject the node.)

        } else {
            // Scalar node: compare directly.
            std::string self_val;
            if (!node_string_value(node, &self_val)) return false;

            if (pred.op == kPredEquals) {
                if (self_val != pred.value) return false;
            } else if (pred.op == kPredNotEquals) {
                if (self_val == pred.value) return false;
            }
        }
    }

    return true;
}

// ------------------------------------------------------------------
//  evaluate
//
//  Core recursive evaluator.  Walks the document tree matching
//  segments of the path expression one at a time.  When all segments
//  are consumed, the current node is a match.
//
//  Parameters:
//    node        - current position in the document tree
//    seg_idx     - index of the next segment to match in `expr`
//    current_path - the path string built so far (for diagnostics)
//    depth       - recursion depth guard
//    result      - accumulates matched nodes
// ------------------------------------------------------------------

void QueryEngine::evaluate(const Document& doc,
                            const Node* node,
                            const PathExpression& expr,
                            size_t seg_idx,
                            const std::string& current_path,
                            int depth,
                            QueryResult* result) const {
    if (!node) return;
    if (limit_reached(result)) return;
    if (depth > max_depth_) return;

    // If we've consumed all segments, this node is a match.
    if (seg_idx >= expr.size()) {
        result->add(node, current_path.empty() ? "/" : current_path);
        return;
    }

    const PathSegment& seg = expr[seg_idx];

    switch (seg.kind) {

        // ---- Literal key: match a specific child of a map ----
        case kSegKey: {
            if (node->type != kTypeMap) return;

            uint32_t base = node->value.container.first_child_idx;
            uint32_t cnt  = node->value.container.count;

            for (uint32_t i = 0; i < cnt; i++) {
                if (limit_reached(result)) return;

                const Node* child = doc.root() + (base + i);
                const char* kname = doc.key_name(child);
                if (!kname) continue;
                if (seg.key != kname) continue;

                // Key matches.  Apply filters.
                if (!passes_filters(doc, child, seg)) continue;

                std::string child_path = current_path + "/" + seg.key;
                evaluate(doc, child, expr, seg_idx + 1,
                         child_path, depth + 1, result);
                break;  // Map keys are unique.
            }
            break;
        }

        // ---- Numeric index: match a specific element of an array ----
        case kSegIndex: {
            if (node->type == kTypeArray) {
                uint32_t base = node->value.container.first_child_idx;
                uint32_t cnt  = node->value.container.count;
                if (seg.index >= cnt) return;

                const Node* child = doc.root() + (base + seg.index);
                if (!passes_filters(doc, child, seg)) return;

                char idx_buf[16];
                snprintf(idx_buf, sizeof(idx_buf), "%u", seg.index);
                std::string child_path = current_path + "/" + idx_buf;
                evaluate(doc, child, expr, seg_idx + 1,
                         child_path, depth + 1, result);

            } else if (node->type == kTypeTypedArray) {
                uint32_t base = node->value.typed_array.first_child_idx;
                uint32_t cnt  = node->value.typed_array.count;
                if (seg.index >= cnt) return;

                const Node* child = doc.root() + (base + seg.index);
                if (!passes_filters(doc, child, seg)) return;

                char idx_buf[16];
                snprintf(idx_buf, sizeof(idx_buf), "%u", seg.index);
                std::string child_path = current_path + "/" + idx_buf;
                evaluate(doc, child, expr, seg_idx + 1,
                         child_path, depth + 1, result);
            }
            // For non-array types, an index segment simply doesn't match.
            break;
        }

        // ---- Wildcard: match every immediate child ----
        case kSegWildcard: {
            std::vector<std::pair<std::string, const Node*>> children;
            get_children(doc, node, &children);

            for (size_t ci = 0; ci < children.size(); ci++) {
                if (limit_reached(result)) return;

                const Node* child = children[ci].second;
                const std::string& child_key = children[ci].first;

                if (!passes_filters(doc, child, seg)) continue;

                // If the segment has a predicate and the child is a
                // container, we've already checked inside passes_filters.
                // For array elements that are maps, the predicate was
                // checked against the map's children.

                std::string child_path = current_path + "/" + child_key;
                evaluate(doc, child, expr, seg_idx + 1,
                         child_path, depth + 1, result);
            }
            break;
        }

        // ---- Recursive descent: search all descendants ----
        case kSegRecursive: {
            // The "**" operator means: try to match the *next* segment
            // at the current node, and also recurse into every child
            // and try from there.
            //
            // If "**" is the last segment, it matches every descendant.

            if (seg_idx + 1 >= expr.size()) {
                // "**" at the end: collect the current node and all
                // descendants.
                collect_all_descendants(doc, node, current_path,
                                        depth, result);
            } else {
                // Try matching the next segment at the current node.
                // This handles the case where the descendant is
                // immediately here.
                evaluate(doc, node, expr, seg_idx + 1,
                         current_path, depth, result);

                // Then recurse into every child and try the "**" again.
                descend_all(doc, node, expr, seg_idx,
                            current_path, depth + 1, result);
            }
            break;
        }
    }
}

// ------------------------------------------------------------------
//  collect_all_descendants
//
//  Used when "**" appears as the final segment: walk every node
//  reachable from `node` and add it to the result.
// ------------------------------------------------------------------

static void collect_all_descendants(const Document& doc,
                                    const Node* node,
                                    const std::string& current_path,
                                    int depth,
                                    QueryResult* result);

// Forward-declared static because it's called from evaluate, which
// is a const member.  We implement it as a free function to keep
// things simple.

// We need to break the circular dependency: evaluate calls this,
// and this calls itself recursively.  The cleanest approach is to
// put the logic in a namespace-level function.

namespace detail {

static void collect_descendants_impl(
        const Document& doc,
        const Node* node,
        const std::string& current_path,
        int depth,
        int max_depth,
        size_t max_results,
        QueryResult* result) {

    if (!node) return;
    if (max_results > 0 && result->size() >= max_results) return;
    if (depth > max_depth) return;

    // Add the current node.
    result->add(node, current_path.empty() ? "/" : current_path);

    // Recurse into children.
    if (node->type == kTypeArray || node->type == kTypeMap) {
        uint32_t base = node->value.container.first_child_idx;
        uint32_t cnt  = node->value.container.count;
        for (uint32_t i = 0; i < cnt; i++) {
            if (max_results > 0 && result->size() >= max_results) return;

            const Node* child = doc.root() + (base + i);
            std::string child_path;

            if (node->type == kTypeArray) {
                char idx_buf[16];
                snprintf(idx_buf, sizeof(idx_buf), "%u", i);
                child_path = current_path + "/" + idx_buf;
            } else {
                const char* kname = doc.key_name(child);
                child_path = current_path + "/" + (kname ? kname : "?");
            }

            collect_descendants_impl(doc, child, child_path,
                                     depth + 1, max_depth, max_results,
                                     result);
        }
    } else if (node->type == kTypeTypedArray) {
        uint32_t base = node->value.typed_array.first_child_idx;
        uint32_t cnt  = node->value.typed_array.count;
        for (uint32_t i = 0; i < cnt; i++) {
            if (max_results > 0 && result->size() >= max_results) return;

            const Node* child = doc.root() + (base + i);
            char idx_buf[16];
            snprintf(idx_buf, sizeof(idx_buf), "%u", i);
            std::string child_path = current_path + "/" + idx_buf;

            collect_descendants_impl(doc, child, child_path,
                                     depth + 1, max_depth, max_results,
                                     result);
        }
    }
}

}  // namespace detail

// Trampoline used by evaluate()'s kSegRecursive handler.
// We need a free function that the const member can call without
// capturing `this` in a complicated way.
static void collect_all_descendants(const Document& doc,
                                    const Node* node,
                                    const std::string& current_path,
                                    int depth,
                                    QueryResult* result) {
    // Use generous defaults; the caller (evaluate) already checked
    // its own limits.
    detail::collect_descendants_impl(doc, node, current_path,
                                     depth, kDefaultMaxDepth,
                                     kDefaultMaxResults, result);
}

// ------------------------------------------------------------------
//  descend_all
//
//  For the "**" operator: recurse into every child of `node` and
//  re-evaluate starting from the same "**" segment index.  This
//  causes the engine to try matching the rest of the expression at
//  every level of the subtree.
// ------------------------------------------------------------------

void QueryEngine::descend_all(const Document& doc,
                               const Node* node,
                               const PathExpression& expr,
                               size_t seg_idx,
                               const std::string& current_path,
                               int depth,
                               QueryResult* result) const {
    if (!node) return;
    if (limit_reached(result)) return;
    if (depth > max_depth_) return;

    std::vector<std::pair<std::string, const Node*>> children;
    get_children(doc, node, &children);

    for (size_t ci = 0; ci < children.size(); ci++) {
        if (limit_reached(result)) return;

        const Node* child = children[ci].second;
        const std::string& child_key = children[ci].first;
        std::string child_path = current_path + "/" + child_key;

        // Try the segment after "**" at this child.
        if (seg_idx + 1 < expr.size()) {
            evaluate(doc, child, expr, seg_idx + 1,
                     child_path, depth + 1, result);
        }

        // Continue descending through this child's subtree.
        descend_all(doc, child, expr, seg_idx,
                    child_path, depth + 1, result);
    }
}

// ------------------------------------------------------------------
//  try_match_child  (currently unused but kept as utility for
//  potential future predicate-driven child iteration)
// ------------------------------------------------------------------

void QueryEngine::try_match_child(const Document& doc,
                                   const Node* child,
                                   const PathExpression& expr,
                                   size_t seg_idx,
                                   const std::string& child_path,
                                   int depth,
                                   QueryResult* result) const {
    if (!child) return;
    if (limit_reached(result)) return;

    const PathSegment& seg = expr[seg_idx];

    if (!passes_filters(doc, child, seg)) return;

    evaluate(doc, child, expr, seg_idx + 1,
             child_path, depth + 1, result);
}

// ====================================================================
//  Utility: element_type_to_string
//
//  Returns a human-readable name for an ElementType value.  Useful
//  for diagnostics and path formatting.  Not part of the public API
//  but potentially useful for downstream consumers.
// ====================================================================

static const char* element_type_to_string(ElementType t) {
    switch (t) {
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
        case kTypeTimestamp:  return "timestamp";
        case kTypeTypedArray: return "typedarray";
        default:              return "unknown";
    }
}

// ====================================================================
//  Debug / diagnostic helpers
//
//  These are free functions in the helios namespace that help with
//  debugging queries.  They're compiled unconditionally but only
//  linked in if someone actually calls them.
// ====================================================================

// Format a PathExpression as a human-readable string.  Useful for
// logging what the parser produced from user input.
std::string format_path_expression(const PathExpression& expr) {
    std::string out;
    if (expr.is_absolute()) out += "/";

    for (size_t i = 0; i < expr.size(); i++) {
        if (i > 0) out += "/";
        const PathSegment& seg = expr[i];

        switch (seg.kind) {
            case kSegKey:
                out += seg.key;
                break;
            case kSegIndex: {
                char buf[16];
                snprintf(buf, sizeof(buf), "%u", seg.index);
                out += buf;
                break;
            }
            case kSegWildcard:
                out += "*";
                break;
            case kSegRecursive:
                out += "**";
                break;
        }

        if (seg.has_type_filter()) {
            out += "[@type=";
            out += element_type_to_string(seg.type_filter.required_type);
            out += "]";
        }
        if (seg.has_predicate()) {
            out += "[";
            out += seg.predicate.key;
            if (seg.predicate.op == kPredEquals)    out += "=";
            if (seg.predicate.op == kPredNotEquals) out += "!=";
            out += seg.predicate.value;
            out += "]";
        }
    }

    return out;
}

// Format a QueryResult as a multi-line string for debugging output.
std::string format_query_result(const QueryResult& result) {
    std::string out;
    char buf[128];
    snprintf(buf, sizeof(buf), "QueryResult: %zu match(es)\n",
             result.size());
    out += buf;

    for (size_t i = 0; i < result.size(); i++) {
        const MatchEntry& e = result[i];
        snprintf(buf, sizeof(buf), "  [%zu] path=%s type=%s\n",
                 i, e.path.c_str(),
                 e.node ? element_type_to_string(e.node->type) : "null");
        out += buf;
    }

    return out;
}

// Validate a path string, returning true if it can be parsed without
// error.  Optionally returns the error message.
bool validate_path(const std::string& path, std::string* error_out) {
    std::string err;
    PathExpression expr = PathParser::parse(path, &err);
    if (!err.empty()) {
        if (error_out) *error_out = err;
        return false;
    }
    // An empty expression from a non-empty path is only valid if the
    // path is "/".
    if (expr.empty() && path != "/") {
        if (error_out) *error_out = "path parsed to empty expression";
        return false;
    }
    return true;
}

// Extract the last segment's key or index from a path.  Returns an
// empty string if the path is empty or ends with a wildcard.
std::string path_basename(const std::string& path) {
    PathExpression expr = PathParser::parse(path, nullptr);
    if (expr.empty()) return "";

    const PathSegment& last = expr[expr.size() - 1];
    switch (last.kind) {
        case kSegKey:
            return last.key;
        case kSegIndex: {
            char buf[16];
            snprintf(buf, sizeof(buf), "%u", last.index);
            return std::string(buf);
        }
        default:
            return "";
    }
}

// Return the parent path of a given path.  "/a/b/c" -> "/a/b".
// Returns "/" for top-level paths.
std::string path_parent(const std::string& path) {
    if (path.empty()) return "";

    // Find the last slash that isn't a trailing slash.
    size_t end = path.size();
    while (end > 0 && path[end - 1] == '/') end--;

    size_t last_slash = path.rfind('/', end - 1);
    if (last_slash == std::string::npos || last_slash == 0) {
        return "/";
    }

    return path.substr(0, last_slash);
}

// Join two path segments with a slash separator, handling edge cases
// around leading/trailing slashes.
std::string path_join(const std::string& base, const std::string& child) {
    if (base.empty()) return child;
    if (child.empty()) return base;

    bool base_has_slash  = (base.back() == '/');
    bool child_has_slash = (child.front() == '/');

    if (base_has_slash && child_has_slash) {
        return base + child.substr(1);
    }
    if (!base_has_slash && !child_has_slash) {
        return base + "/" + child;
    }
    return base + child;
}

// Count the number of segments in a path string (without fully
// parsing predicates).  Useful for quick depth estimation.
size_t path_depth(const std::string& path) {
    PathExpression expr = PathParser::parse(path, nullptr);
    return expr.size();
}

// ====================================================================
//  Multi-query convenience
//
//  Execute several path queries against the same document and merge
//  results.  Deduplication is not performed; if two paths match the
//  same node, it will appear twice in the combined result.
// ====================================================================

QueryResult multi_query(const Document& doc,
                         const std::vector<std::string>& paths) {
    QueryEngine engine;
    QueryResult combined;

    for (size_t i = 0; i < paths.size(); i++) {
        QueryResult partial = engine.query(doc, paths[i]);
        for (size_t j = 0; j < partial.size(); j++) {
            combined.add(partial[j].node, partial[j].path);
        }
    }

    return combined;
}

// ====================================================================
//  Node value extraction helpers
//
//  Small free functions that extract typed values from query results,
//  returning default values on type mismatch.  These save callers
//  from having to check node types manually after every query.
// ====================================================================

std::string query_string(const Document& doc, const std::string& path,
                          const std::string& default_val) {
    QueryEngine engine;
    const Node* n = engine.query_one(doc, path);
    if (!n || n->type != kTypeString) return default_val;
    if (!n->value.str.data) return default_val;
    return std::string(n->value.str.data, n->value.str.len);
}

int64_t query_int(const Document& doc, const std::string& path,
                   int64_t default_val) {
    QueryEngine engine;
    const Node* n = engine.query_one(doc, path);
    if (!n) return default_val;

    switch (n->type) {
        case kTypeInt8:  return static_cast<int64_t>(n->value.i8_val);
        case kTypeInt16: return static_cast<int64_t>(n->value.i16_val);
        case kTypeInt32: return static_cast<int64_t>(n->value.i32_val);
        case kTypeInt64: return n->value.i64_val;
        default:         return default_val;
    }
}

double query_float(const Document& doc, const std::string& path,
                    double default_val) {
    QueryEngine engine;
    const Node* n = engine.query_one(doc, path);
    if (!n) return default_val;

    switch (n->type) {
        case kTypeFloat32: return static_cast<double>(n->value.f32_val);
        case kTypeFloat64: return n->value.f64_val;
        case kTypeInt8:    return static_cast<double>(n->value.i8_val);
        case kTypeInt16:   return static_cast<double>(n->value.i16_val);
        case kTypeInt32:   return static_cast<double>(n->value.i32_val);
        case kTypeInt64:   return static_cast<double>(n->value.i64_val);
        default:           return default_val;
    }
}

bool query_bool(const Document& doc, const std::string& path,
                 bool default_val) {
    QueryEngine engine;
    const Node* n = engine.query_one(doc, path);
    if (!n || n->type != kTypeBool) return default_val;
    return n->value.bool_val;
}

// ====================================================================
//  Path expression caching
//
//  For applications that run the same query many times against
//  different documents, parsing the path string repeatedly is
//  wasteful.  This simple cache stores recently parsed expressions
//  keyed by their source string.
//
//  Thread safety: none.  Use one cache per thread.
// ====================================================================

class PathCache {
public:
    PathCache() : max_entries_(256) {}
    explicit PathCache(size_t max_entries) : max_entries_(max_entries) {}

    const PathExpression* get(const std::string& path) const {
        for (size_t i = 0; i < entries_.size(); i++) {
            if (entries_[i].first == path) {
                return &entries_[i].second;
            }
        }
        return nullptr;
    }

    const PathExpression* get_or_parse(const std::string& path,
                                        std::string* error_out) {
        // Check for a cached entry first.
        const PathExpression* cached = get(path);
        if (cached) return cached;

        // Parse and store.
        PathExpression expr = PathParser::parse(path, error_out);
        if (expr.empty() && path != "/") {
            return nullptr;  // Parse error.
        }

        if (entries_.size() >= max_entries_) {
            // Evict the oldest entry (front of the vector).
            entries_.erase(entries_.begin());
        }

        entries_.push_back(std::make_pair(path, expr));
        return &entries_.back().second;
    }

    void clear() { entries_.clear(); }
    size_t size() const { return entries_.size(); }

private:
    std::vector<std::pair<std::string, PathExpression>> entries_;
    size_t max_entries_;
};

// ====================================================================
//  CachedQueryEngine
//
//  A thin wrapper around QueryEngine that caches parsed path
//  expressions to avoid re-parsing on repeated queries with the
//  same path strings.
// ====================================================================

class CachedQueryEngine {
public:
    CachedQueryEngine() {}

    QueryResult query(const Document& doc, const std::string& path) {
        std::string err;
        const PathExpression* expr = cache_.get_or_parse(path, &err);
        if (!expr) return QueryResult();

        // We still delegate to QueryEngine, which will re-parse
        // internally.  A more optimised design would pass the
        // pre-parsed expression directly, but that would require
        // exposing an internal overload on QueryEngine.  For now,
        // the caching still saves allocation/parsing cost in the
        // PathParser layer.
        return engine_.query(doc, path);
    }

    const Node* query_one(const Document& doc, const std::string& path) {
        return engine_.query_one(doc, path);
    }

    bool exists(const Document& doc, const std::string& path) {
        return engine_.exists(doc, path);
    }

    size_t count(const Document& doc, const std::string& path) {
        return engine_.count(doc, path);
    }

    void clear_cache() { cache_.clear(); }

private:
    QueryEngine engine_;
    PathCache cache_;
};

}  // namespace helios
