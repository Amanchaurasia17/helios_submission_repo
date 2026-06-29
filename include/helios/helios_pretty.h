
#ifndef HELIOS_PRETTY_H_
#define HELIOS_PRETTY_H_

#include "helios_types.h"
#include "helios_node.h"
#include "helios_document.h"
#include "helios_string_table.h"

#include <cstdint>
#include <cstddef>
#include <string>

namespace helios {

// ---------------------------------------------------------------------------
// PrintConfig -- knobs for controlling pretty-printer output.
// ---------------------------------------------------------------------------
struct PrintConfig {
    // Basic formatting
    bool     use_tabs;         // true = indent with '\t', false = spaces
    int      indent_width;     // spaces per level (ignored when use_tabs=true)
    int      max_depth;        // stop recursing below this depth  (-1 = unlimited)
    size_t   max_string_len;   // truncate strings longer than this (0 = no limit)

    // Content toggles
    bool     show_type_annotations;  // "[int32] 42" vs just "42"
    bool     show_key_indices;       // show string-table index next to key names
    bool     compact_small;          // single-line arrays/maps with <= compact_threshold children
    int      compact_threshold;      // child count for compact mode

    // Hex dump
    int      hex_bytes_per_line;     // bytes per row in hex dumps (default 16)

    // Color (ANSI escape sequences)
    bool     colorize;               // emit ANSI color codes

    PrintConfig();  // sets sensible defaults
};

// ---------------------------------------------------------------------------
// FormatBuffer -- lightweight string builder that tracks indentation.
// Not meant for external use, but exposed for testing convenience.
// ---------------------------------------------------------------------------
class FormatBuffer {
public:
    explicit FormatBuffer(const PrintConfig& cfg);
    ~FormatBuffer();

    void append(const char* text);
    void append(const char* text, size_t len);
    void append_char(char c);
    void append_int(int64_t val);
    void append_uint(uint64_t val);
    void append_double(double val);
    void append_hex_byte(uint8_t byte);
    void newline();
    void indent();
    void dedent();
    void write_indent();

    // ANSI helpers -- no-ops when colorize is off.
    void color_key();
    void color_value();
    void color_type();
    void color_null();
    void color_string();
    void color_number();
    void color_reset();

    std::string to_string() const;
    size_t length() const { return len_; }
    void clear();

private:
    void ensure_capacity(size_t additional);
    void append_raw(const char* s, size_t n);

    const PrintConfig& cfg_;
    char*  buf_;
    size_t len_;
    size_t cap_;
    int    depth_;
};

// ---------------------------------------------------------------------------
// DocumentStats -- aggregate info collected from a single document.
// ---------------------------------------------------------------------------
struct DocumentStats {
    size_t total_nodes;
    int    max_depth_seen;
    size_t total_string_bytes;   // sum of all string value lengths
    size_t total_blob_bytes;     // sum of all blob payloads

    // Per-type element counts (indexed by ElementType value).
    // Only the first 32 slots are used; indices beyond that are ignored.
    size_t type_counts[32];

    DocumentStats();
    void reset();
    std::string summary() const;
};

// ---------------------------------------------------------------------------
// PrettyPrinter -- main entry point.
// ---------------------------------------------------------------------------
class PrettyPrinter {
public:
    PrettyPrinter();
    explicit PrettyPrinter(const PrintConfig& cfg);
    ~PrettyPrinter();

    // --- full-document formatters ---
    std::string to_json(const Document& doc) const;
    std::string to_xml(const Document& doc) const;
    std::string to_yaml(const Document& doc) const;

    // --- utility formatters ---
    std::string dump_hex(const uint8_t* data, size_t len) const;
    std::string format_node(const Node* node, const Document& doc) const;
    std::string format_value(const Node* node, const Document& doc) const;

    // --- statistics ---
    DocumentStats collect_stats(const Document& doc) const;

    // --- configuration ---
    void set_config(const PrintConfig& cfg);
    const PrintConfig& config() const { return cfg_; }

private:
    // JSON recursive workers
    void json_emit_node(const Node* node, const Document& doc,
                        FormatBuffer& buf, int depth) const;
    void json_emit_value(const Node* node, const Document& doc,
                         FormatBuffer& buf, int depth) const;
    void json_emit_array(const Node* node, const Document& doc,
                         FormatBuffer& buf, int depth) const;
    void json_emit_map(const Node* node, const Document& doc,
                       FormatBuffer& buf, int depth) const;
    bool json_should_compact(const Node* node, const Document& doc) const;

    // XML recursive workers
    void xml_emit_node(const Node* node, const Document& doc,
                       FormatBuffer& buf, int depth,
                       const char* tag_override) const;
    void xml_emit_children(const Node* node, const Document& doc,
                           FormatBuffer& buf, int depth) const;

    // YAML recursive workers
    void yaml_emit_node(const Node* node, const Document& doc,
                        FormatBuffer& buf, int depth, bool is_list_item) const;
    void yaml_emit_value(const Node* node, const Document& doc,
                         FormatBuffer& buf) const;
    void yaml_emit_array(const Node* node, const Document& doc,
                         FormatBuffer& buf, int depth) const;
    void yaml_emit_map(const Node* node, const Document& doc,
                       FormatBuffer& buf, int depth) const;

    // Stats walker
    void walk_stats(const Node* node, const Document& doc,
                    DocumentStats& stats, int depth) const;

    // Helpers
    static const char* type_name(ElementType t);
    void append_escaped_string(FormatBuffer& buf, const char* s,
                               size_t len) const;
    void append_truncated_string(FormatBuffer& buf, const char* s,
                                 size_t len) const;

    PrintConfig cfg_;
};

}  // namespace helios

#endif  // HELIOS_PRETTY_H_
