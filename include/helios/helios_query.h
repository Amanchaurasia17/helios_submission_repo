
#ifndef HELIOS_QUERY_H_
#define HELIOS_QUERY_H_

#include "helios_types.h"
#include "helios_node.h"
#include "helios_document.h"
#include <string>
#include <vector>
#include <cstddef>

namespace helios {

// --------------------------------------------------------------------
// PathSegment
//
// Represents a single segment in a path expression, e.g. the "child"
// in "/root/child".  Segments can be literal keys, array indices,
// wildcards ("*"), or recursive descent markers ("**").  Each segment
// may optionally carry a predicate filter or a type filter.
// --------------------------------------------------------------------

enum SegmentKind {
    kSegKey,          // Matches a map key by name.
    kSegIndex,        // Matches an array element by numeric index.
    kSegWildcard,     // Matches any single child ("*").
    kSegRecursive,    // Matches any descendant at any depth ("**").
};

// Predicate operators for bracket-filter expressions.
enum PredicateOp {
    kPredNone,        // No predicate.
    kPredEquals,      // [key=value]
    kPredNotEquals,   // [key!=value]
};

struct Predicate {
    PredicateOp op;
    std::string key;
    std::string value;

    Predicate() : op(kPredNone) {}
};

// Type filter, expressed as [@type=<name>] in the path.
struct TypeFilter {
    bool active;
    ElementType required_type;

    TypeFilter() : active(false), required_type(kTypeNull) {}
};

struct PathSegment {
    SegmentKind  kind;
    std::string  key;       // For kSegKey.
    uint32_t     index;     // For kSegIndex.
    Predicate    predicate; // Optional [key=value] filter.
    TypeFilter   type_filter; // Optional [@type=string] filter.

    PathSegment()
        : kind(kSegKey), index(0) {}

    bool has_predicate() const { return predicate.op != kPredNone; }
    bool has_type_filter() const { return type_filter.active; }
};

// --------------------------------------------------------------------
// PathExpression
//
// An ordered list of PathSegments, parsed from a path string such as
// "/root/children/*/name" or "/data/**/[@type=string]".
// The expression records whether it was specified as absolute (leading
// slash) so the engine can decide if the root must match.
// --------------------------------------------------------------------

class PathExpression {
public:
    PathExpression();

    const std::vector<PathSegment>& segments() const { return segments_; }
    bool is_absolute() const { return absolute_; }
    bool empty() const { return segments_.empty(); }
    size_t size() const { return segments_.size(); }
    const PathSegment& operator[](size_t i) const { return segments_[i]; }

private:
    friend class PathParser;
    std::vector<PathSegment> segments_;
    bool absolute_;
};

// --------------------------------------------------------------------
// PathParser
//
// Stateless utility that converts a path string into a PathExpression.
// Syntax:
//   /root/child        - absolute path through map keys
//   root/child         - relative path
//   /array/0/field     - index into array element 0
//   /data/*            - wildcard, matches any direct child
//   /data/**           - recursive descent into all descendants
//   /items[key=val]    - predicate filter on array elements
//   /items[@type=str]  - type filter
// On parse failure the returned expression is empty and the error
// string is populated.
// --------------------------------------------------------------------

class PathParser {
public:
    static PathExpression parse(const std::string& path,
                                std::string* error_out = nullptr);

private:
    // Helpers: these operate on substrings to avoid copying.
    static bool parse_segment(const char* begin, const char* end,
                              PathSegment* out, std::string* error_out);
    static bool parse_predicate(const char* begin, const char* end,
                                Predicate* out, std::string* error_out);
    static bool parse_type_filter(const char* begin, const char* end,
                                  TypeFilter* out, std::string* error_out);
    static bool is_numeric(const char* begin, const char* end);
    static ElementType type_name_to_enum(const char* name, size_t len);
};

// --------------------------------------------------------------------
// QueryResult
//
// Holds the result set of a query: zero or more matched nodes together
// with the string path that led to each match.
// --------------------------------------------------------------------

struct MatchEntry {
    const Node*  node;
    std::string  path;  // Reconstructed path string, e.g. "/root/child/0".
};

class QueryResult {
public:
    QueryResult() {}

    bool empty() const { return entries_.empty(); }
    size_t size() const { return entries_.size(); }

    const MatchEntry& operator[](size_t i) const { return entries_[i]; }
    const std::vector<MatchEntry>& entries() const { return entries_; }

    // Convenience: returns the first match, or nullptr.
    const Node* first_node() const;
    const std::string& first_path() const;

    void add(const Node* n, const std::string& path);
    void clear();

private:
    std::vector<MatchEntry> entries_;
};

// --------------------------------------------------------------------
// QueryEngine
//
// Executes path-based queries against a parsed Document.
//
// Usage:
//   QueryEngine qe;
//   QueryResult r = qe.query(doc, "/config/server/host");
//   if (!r.empty())
//       handle(r[0].node);
//
//   const Node* one = qe.query_one(doc, "/list/0/name");
//   bool found      = qe.exists(doc, "/list/**/id");
//   size_t n        = qe.count(doc, "/users/*");
// --------------------------------------------------------------------

class QueryEngine {
public:
    QueryEngine();

    // Execute a query, returning all matching nodes.
    QueryResult query(const Document& doc,
                      const std::string& path) const;

    // Execute a query, returning only the first match (or nullptr).
    const Node* query_one(const Document& doc,
                          const std::string& path) const;

    // Returns true if at least one node matches.
    bool exists(const Document& doc,
                const std::string& path) const;

    // Returns the number of matching nodes.
    size_t count(const Document& doc,
                 const std::string& path) const;

    // Maximum number of results before the engine stops collecting.
    // Set to 0 for unlimited.
    void set_max_results(size_t limit) { max_results_ = limit; }
    size_t max_results() const { return max_results_; }

    // Maximum depth during recursive descent to avoid runaway
    // traversal on pathological trees.
    void set_max_depth(int depth) { max_depth_ = depth; }
    int max_depth() const { return max_depth_; }

private:
    // Recursive core of the query evaluator.
    void evaluate(const Document& doc,
                  const Node* node,
                  const PathExpression& expr,
                  size_t seg_idx,
                  const std::string& current_path,
                  int depth,
                  QueryResult* result) const;

    // Try matching a single segment against a specific child.
    void try_match_child(const Document& doc,
                         const Node* child,
                         const PathExpression& expr,
                         size_t seg_idx,
                         const std::string& child_path,
                         int depth,
                         QueryResult* result) const;

    // Recursive-descent helper: walk every descendant.
    void descend_all(const Document& doc,
                     const Node* node,
                     const PathExpression& expr,
                     size_t seg_idx,
                     const std::string& current_path,
                     int depth,
                     QueryResult* result) const;

    // Check if a node passes the predicate and type filters
    // attached to a segment.
    bool passes_filters(const Document& doc,
                        const Node* node,
                        const PathSegment& seg) const;

    // Resolve a node's string value for predicate comparison.
    bool node_string_value(const Node* node,
                           std::string* out) const;

    // Get children of a container node.
    void get_children(const Document& doc,
                      const Node* node,
                      std::vector<std::pair<std::string, const Node*>>* out) const;

    // Check whether we should stop collecting results.
    bool limit_reached(const QueryResult* result) const;

    size_t max_results_;
    int max_depth_;
};

}  // namespace helios

#endif  // HELIOS_QUERY_H_
