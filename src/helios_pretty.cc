
// helios_pretty.cc  --  pretty-printer / debug dump for FBF documents
//
// Converts a parsed FBF Document tree into human-readable text in one of
// several formats (JSON-like, XML-like, YAML-like) with optional ANSI
// coloring, type annotations, indentation control and max-depth limiting.
//
// This file intentionally avoids std::regex, std::optional, and other C++17
// features so it compiles cleanly under -std=c++14.

#include "helios_pretty.h"

#include <cassert>
#include <cctype>
#include <cinttypes>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace helios {

// ===================================================================
// ANSI escape code constants
// ===================================================================
static const char* kAnsiReset   = "\033[0m";
static const char* kAnsiKey     = "\033[1;36m";   // bold cyan
static const char* kAnsiValue   = "\033[0;37m";   // white
static const char* kAnsiType    = "\033[0;90m";   // dark grey
static const char* kAnsiNull    = "\033[1;31m";   // bold red
static const char* kAnsiString  = "\033[0;32m";   // green
static const char* kAnsiNumber  = "\033[0;33m";   // yellow

// ===================================================================
// PrintConfig
// ===================================================================
PrintConfig::PrintConfig()
    : use_tabs(false)
    , indent_width(2)
    , max_depth(-1)
    , max_string_len(0)
    , show_type_annotations(false)
    , show_key_indices(false)
    , compact_small(true)
    , compact_threshold(4)
    , hex_bytes_per_line(16)
    , colorize(false) {
}

// ===================================================================
// FormatBuffer
// ===================================================================
static const size_t kInitialBufSize = 1024;

FormatBuffer::FormatBuffer(const PrintConfig& cfg)
    : cfg_(cfg)
    , buf_(nullptr)
    , len_(0)
    , cap_(0)
    , depth_(0) {
    buf_ = static_cast<char*>(std::malloc(kInitialBufSize));
    if (buf_) {
        cap_ = kInitialBufSize;
    }
}

FormatBuffer::~FormatBuffer() {
    std::free(buf_);
}

void FormatBuffer::ensure_capacity(size_t additional) {
    if (len_ + additional + 1 <= cap_) {
        return;
    }
    size_t needed = len_ + additional + 1;
    size_t new_cap = cap_ ? cap_ * 2 : kInitialBufSize;
    while (new_cap < needed) {
        new_cap *= 2;
    }
    char* tmp = static_cast<char*>(std::realloc(buf_, new_cap));
    if (tmp) {
        buf_ = tmp;
        cap_ = new_cap;
    }
    // If realloc fails we just keep the old buffer and hope for the best.
    // In practice this will cause truncated output rather than a crash.
}

void FormatBuffer::append_raw(const char* s, size_t n) {
    ensure_capacity(n);
    if (len_ + n <= cap_) {
        std::memcpy(buf_ + len_, s, n);
        len_ += n;
    }
}

void FormatBuffer::append(const char* text) {
    if (!text) return;
    append_raw(text, std::strlen(text));
}

void FormatBuffer::append(const char* text, size_t len) {
    if (!text) return;
    append_raw(text, len);
}

void FormatBuffer::append_char(char c) {
    ensure_capacity(1);
    if (len_ < cap_) {
        buf_[len_++] = c;
    }
}

void FormatBuffer::append_int(int64_t val) {
    char tmp[32];
    int n = std::snprintf(tmp, sizeof(tmp), "%" PRId64, val);
    if (n > 0) {
        append_raw(tmp, static_cast<size_t>(n));
    }
}

void FormatBuffer::append_uint(uint64_t val) {
    char tmp[32];
    int n = std::snprintf(tmp, sizeof(tmp), "%" PRIu64, val);
    if (n > 0) {
        append_raw(tmp, static_cast<size_t>(n));
    }
}

void FormatBuffer::append_double(double val) {
    char tmp[64];
    // Use %g for compact representation; fall back to full precision
    // if the result would round-trip incorrectly.
    int n = std::snprintf(tmp, sizeof(tmp), "%.17g", val);
    if (n > 0) {
        append_raw(tmp, static_cast<size_t>(n));
    }
}

void FormatBuffer::append_hex_byte(uint8_t byte) {
    static const char hex_chars[] = "0123456789abcdef";
    char pair[2];
    pair[0] = hex_chars[(byte >> 4) & 0x0F];
    pair[1] = hex_chars[byte & 0x0F];
    append_raw(pair, 2);
}

void FormatBuffer::newline() {
    append_char('\n');
}

void FormatBuffer::indent() {
    ++depth_;
}

void FormatBuffer::dedent() {
    if (depth_ > 0) --depth_;
}

void FormatBuffer::write_indent() {
    if (depth_ <= 0) return;
    if (cfg_.use_tabs) {
        for (int i = 0; i < depth_; ++i) {
            append_char('\t');
        }
    } else {
        int total = depth_ * cfg_.indent_width;
        for (int i = 0; i < total; ++i) {
            append_char(' ');
        }
    }
}

// ANSI color helpers -----------------------------------------------
void FormatBuffer::color_key()    { if (cfg_.colorize) append(kAnsiKey); }
void FormatBuffer::color_value()  { if (cfg_.colorize) append(kAnsiValue); }
void FormatBuffer::color_type()   { if (cfg_.colorize) append(kAnsiType); }
void FormatBuffer::color_null()   { if (cfg_.colorize) append(kAnsiNull); }
void FormatBuffer::color_string() { if (cfg_.colorize) append(kAnsiString); }
void FormatBuffer::color_number() { if (cfg_.colorize) append(kAnsiNumber); }
void FormatBuffer::color_reset()  { if (cfg_.colorize) append(kAnsiReset); }

std::string FormatBuffer::to_string() const {
    if (!buf_ || len_ == 0) {
        return std::string();
    }
    return std::string(buf_, len_);
}

void FormatBuffer::clear() {
    len_ = 0;
    depth_ = 0;
}

// ===================================================================
// DocumentStats
// ===================================================================
DocumentStats::DocumentStats() {
    reset();
}

void DocumentStats::reset() {
    total_nodes = 0;
    max_depth_seen = 0;
    total_string_bytes = 0;
    total_blob_bytes = 0;
    std::memset(type_counts, 0, sizeof(type_counts));
}

std::string DocumentStats::summary() const {
    // Build a plain-text summary of collected stats.
    char line[256];
    std::string out;
    out.reserve(512);

    std::snprintf(line, sizeof(line), "Document Statistics\n");
    out += line;
    std::snprintf(line, sizeof(line), "  total nodes       : %zu\n", total_nodes);
    out += line;
    std::snprintf(line, sizeof(line), "  max depth         : %d\n", max_depth_seen);
    out += line;
    std::snprintf(line, sizeof(line), "  total string bytes: %zu\n", total_string_bytes);
    out += line;
    std::snprintf(line, sizeof(line), "  total blob bytes  : %zu\n", total_blob_bytes);
    out += line;

    // Per-type breakdown.  Only print types that appear at least once.
    static const struct { ElementType t; const char* name; } kTypes[] = {
        { kTypeNull,       "null"        },
        { kTypeBool,       "bool"        },
        { kTypeInt8,       "int8"        },
        { kTypeInt16,      "int16"       },
        { kTypeInt32,      "int32"       },
        { kTypeInt64,      "int64"       },
        { kTypeFloat32,    "float32"     },
        { kTypeFloat64,    "float64"     },
        { kTypeString,     "string"      },
        { kTypeBlob,       "blob"        },
        { kTypeArray,      "array"       },
        { kTypeMap,        "map"         },
        { kTypeCompressed, "compressed"  },
        { kTypeReference,  "reference"   },
        { kTypeTimestamp,  "timestamp"   },
        { kTypeTypedArray, "typed_array" },
    };

    bool any_types = false;
    for (size_t i = 0; i < sizeof(kTypes) / sizeof(kTypes[0]); ++i) {
        size_t idx = static_cast<size_t>(kTypes[i].t);
        if (idx < 32 && type_counts[idx] > 0) {
            if (!any_types) {
                out += "  element counts:\n";
                any_types = true;
            }
            std::snprintf(line, sizeof(line), "    %-14s: %zu\n",
                          kTypes[i].name, type_counts[idx]);
            out += line;
        }
    }
    if (!any_types) {
        out += "  (no elements)\n";
    }
    return out;
}

// ===================================================================
// PrettyPrinter -- construction / config
// ===================================================================
PrettyPrinter::PrettyPrinter() : cfg_() {}

PrettyPrinter::PrettyPrinter(const PrintConfig& cfg) : cfg_(cfg) {}

PrettyPrinter::~PrettyPrinter() {}

void PrettyPrinter::set_config(const PrintConfig& cfg) {
    cfg_ = cfg;
}

// ===================================================================
// type_name -- ElementType to human string
// ===================================================================
const char* PrettyPrinter::type_name(ElementType t) {
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
        case kTypeTypedArray: return "typed_array";
        default:              return "unknown";
    }
}

// ===================================================================
// String escaping helpers
// ===================================================================
void PrettyPrinter::append_escaped_string(FormatBuffer& buf,
                                           const char* s,
                                           size_t len) const {
    buf.append_char('"');
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        switch (c) {
            case '"':  buf.append("\\\""); break;
            case '\\': buf.append("\\\\"); break;
            case '\b': buf.append("\\b");  break;
            case '\f': buf.append("\\f");  break;
            case '\n': buf.append("\\n");  break;
            case '\r': buf.append("\\r");  break;
            case '\t': buf.append("\\t");  break;
            default:
                if (c < 0x20) {
                    // Control character -- emit as \u00XX.
                    char esc[8];
                    std::snprintf(esc, sizeof(esc), "\\u%04x",
                                  static_cast<unsigned>(c));
                    buf.append(esc);
                } else {
                    buf.append_char(static_cast<char>(c));
                }
                break;
        }
    }
    buf.append_char('"');
}

void PrettyPrinter::append_truncated_string(FormatBuffer& buf,
                                             const char* s,
                                             size_t len) const {
    if (cfg_.max_string_len > 0 && len > cfg_.max_string_len) {
        append_escaped_string(buf, s, cfg_.max_string_len);
        // Replace the closing quote with a truncation marker.
        // This is a bit of a hack: we pop the final '"' and append instead.
        // FormatBuffer doesn't support pop, so we just append the marker
        // right after the closing quote.
        char marker[32];
        std::snprintf(marker, sizeof(marker), "...(%zu more)",
                      len - cfg_.max_string_len);
        buf.append(marker);
    } else {
        append_escaped_string(buf, s, len);
    }
}

// ===================================================================
// dump_hex -- hex dump of raw bytes
// ===================================================================
std::string PrettyPrinter::dump_hex(const uint8_t* data, size_t len) const {
    if (!data || len == 0) {
        return "(empty)\n";
    }

    FormatBuffer buf(cfg_);
    int per_line = cfg_.hex_bytes_per_line;
    if (per_line <= 0) per_line = 16;

    for (size_t offset = 0; offset < len; offset += static_cast<size_t>(per_line)) {
        // Address column
        char addr[16];
        std::snprintf(addr, sizeof(addr), "%08zx  ", offset);
        buf.append(addr);

        // Hex bytes
        size_t row_end = offset + static_cast<size_t>(per_line);
        if (row_end > len) row_end = len;

        for (size_t i = offset; i < row_end; ++i) {
            buf.append_hex_byte(data[i]);
            buf.append_char(' ');
            // Extra space in the middle for readability (like xxd).
            if ((i - offset) == static_cast<size_t>(per_line / 2 - 1)) {
                buf.append_char(' ');
            }
        }

        // Pad incomplete last row so the ASCII column lines up.
        for (size_t i = row_end; i < offset + static_cast<size_t>(per_line); ++i) {
            buf.append("   ");
            if ((i - offset) == static_cast<size_t>(per_line / 2 - 1)) {
                buf.append_char(' ');
            }
        }

        buf.append(" |");

        // ASCII column
        for (size_t i = offset; i < row_end; ++i) {
            char c = static_cast<char>(data[i]);
            if (std::isprint(static_cast<unsigned char>(c))) {
                buf.append_char(c);
            } else {
                buf.append_char('.');
            }
        }

        buf.append_char('|');
        buf.newline();
    }

    return buf.to_string();
}

// ===================================================================
// collect_stats
// ===================================================================
DocumentStats PrettyPrinter::collect_stats(const Document& doc) const {
    DocumentStats stats;
    if (!doc.is_valid() || !doc.root()) {
        return stats;
    }
    walk_stats(doc.root(), doc, stats, 0);
    return stats;
}

void PrettyPrinter::walk_stats(const Node* node, const Document& doc,
                                DocumentStats& stats, int depth) const {
    if (!node) return;

    stats.total_nodes++;
    if (depth > stats.max_depth_seen) {
        stats.max_depth_seen = depth;
    }

    size_t type_idx = static_cast<size_t>(node->type);
    if (type_idx < 32) {
        stats.type_counts[type_idx]++;
    }

    switch (node->type) {
        case kTypeString:
            if (node->value.str.data) {
                stats.total_string_bytes += node->value.str.len;
            }
            break;

        case kTypeBlob:
            if (node->value.str.data) {
                stats.total_blob_bytes += node->value.str.len;
            }
            break;

        case kTypeArray:
        case kTypeMap: {
            // Walk children.  The parser stores them as a contiguous block
            // starting at first_child_idx.
            uint32_t base = node->value.container.first_child_idx;
            uint32_t cnt  = node->value.container.count;
            for (uint32_t i = 0; i < cnt; ++i) {
                // We need a way to get child nodes.  Document doesn't
                // expose the pool directly, but find() and root() let
                // us navigate.  For stats we cheat a bit and use the
                // node_count() to bound-check, then cast away constness
                // to access pool indices.  This is safe because we only
                // read.
                //
                // In a real codebase, Document would expose a
                //   const Node* child(uint32_t index) const;
                // method.  Here we reconstruct access from the root
                // address and pointer arithmetic.
                const Node* root = doc.root();
                if (!root) break;
                // Nodes are stored contiguously, so we can offset from
                // the first node.  root is pool_.get(root_node_index_)
                // and root_node_index_ is typically 0 after parsing.
                const Node* child = root + (base + i);
                // Safety: make sure we don't walk past node_count.
                if (static_cast<size_t>(base + i) >= doc.node_count()) {
                    break;
                }
                walk_stats(child, doc, stats, depth + 1);
            }
            break;
        }

        case kTypeTypedArray: {
            uint32_t base = node->value.typed_array.first_child_idx;
            uint32_t cnt  = node->value.typed_array.count;
            for (uint32_t i = 0; i < cnt; ++i) {
                const Node* root = doc.root();
                if (!root) break;
                const Node* child = root + (base + i);
                if (static_cast<size_t>(base + i) >= doc.node_count()) {
                    break;
                }
                walk_stats(child, doc, stats, depth + 1);
            }
            break;
        }

        default:
            // Scalars -- nothing more to do.
            break;
    }
}

// ===================================================================
// format_node / format_value  -- single-node formatters
// ===================================================================
std::string PrettyPrinter::format_node(const Node* node,
                                        const Document& doc) const {
    if (!node) return "(null node)";
    FormatBuffer buf(cfg_);
    json_emit_node(node, doc, buf, 0);
    return buf.to_string();
}

std::string PrettyPrinter::format_value(const Node* node,
                                         const Document& doc) const {
    if (!node) return "(null node)";
    FormatBuffer buf(cfg_);
    json_emit_value(node, doc, buf, 0);
    return buf.to_string();
}

// ===================================================================
//  JSON output
// ===================================================================
std::string PrettyPrinter::to_json(const Document& doc) const {
    if (!doc.is_valid()) {
        return "/* invalid document */\n";
    }
    const Node* root = doc.root();
    if (!root) {
        return "/* empty document */\n";
    }

    FormatBuffer buf(cfg_);
    json_emit_node(root, doc, buf, 0);
    buf.newline();
    return buf.to_string();
}

// Decide if a container node is small enough for compact (single-line) output.
bool PrettyPrinter::json_should_compact(const Node* node,
                                         const Document& doc) const {
    if (!cfg_.compact_small) return false;
    if (!node) return false;

    uint32_t count = 0;
    if (node->type == kTypeArray || node->type == kTypeMap) {
        count = node->value.container.count;
    } else if (node->type == kTypeTypedArray) {
        count = node->value.typed_array.count;
    } else {
        return true;  // scalars are always "compact"
    }

    if (count > static_cast<uint32_t>(cfg_.compact_threshold)) {
        return false;
    }

    // For maps, each child might itself be a container; skip compact if so.
    const Node* root = doc.root();
    if (!root) return false;

    uint32_t base = (node->type == kTypeTypedArray)
                    ? node->value.typed_array.first_child_idx
                    : node->value.container.first_child_idx;

    for (uint32_t i = 0; i < count; ++i) {
        if (static_cast<size_t>(base + i) >= doc.node_count()) break;
        const Node* child = root + (base + i);
        if (child->type == kTypeArray || child->type == kTypeMap ||
            child->type == kTypeTypedArray) {
            return false;
        }
    }
    return true;
}

void PrettyPrinter::json_emit_node(const Node* node, const Document& doc,
                                    FormatBuffer& buf, int depth) const {
    if (!node) {
        buf.color_null();
        buf.append("null");
        buf.color_reset();
        return;
    }

    // Depth limiting
    if (cfg_.max_depth >= 0 && depth > cfg_.max_depth) {
        buf.color_type();
        buf.append("/* ... depth limit ... */");
        buf.color_reset();
        return;
    }

    // Optional type annotation
    if (cfg_.show_type_annotations) {
        buf.color_type();
        buf.append_char('[');
        buf.append(type_name(node->type));
        buf.append("] ");
        buf.color_reset();
    }

    json_emit_value(node, doc, buf, depth);
}

void PrettyPrinter::json_emit_value(const Node* node, const Document& doc,
                                     FormatBuffer& buf, int depth) const {
    switch (node->type) {
        case kTypeNull:
            buf.color_null();
            buf.append("null");
            buf.color_reset();
            break;

        case kTypeBool:
            buf.color_number();
            buf.append(node->value.bool_val ? "true" : "false");
            buf.color_reset();
            break;

        case kTypeInt8:
            buf.color_number();
            buf.append_int(static_cast<int64_t>(node->value.i8_val));
            buf.color_reset();
            break;

        case kTypeInt16:
            buf.color_number();
            buf.append_int(static_cast<int64_t>(node->value.i16_val));
            buf.color_reset();
            break;

        case kTypeInt32:
            buf.color_number();
            buf.append_int(static_cast<int64_t>(node->value.i32_val));
            buf.color_reset();
            break;

        case kTypeInt64:
            buf.color_number();
            buf.append_int(node->value.i64_val);
            buf.color_reset();
            break;

        case kTypeFloat32:
            buf.color_number();
            buf.append_double(static_cast<double>(node->value.f32_val));
            buf.color_reset();
            break;

        case kTypeFloat64:
            buf.color_number();
            buf.append_double(node->value.f64_val);
            buf.color_reset();
            break;

        case kTypeString:
            buf.color_string();
            if (node->value.str.data) {
                append_truncated_string(buf, node->value.str.data,
                                        node->value.str.len);
            } else {
                buf.append("\"\"");
            }
            buf.color_reset();
            break;

        case kTypeBlob: {
            // Emit as a JSON string with hex-encoded content, like
            // "blob(12 bytes): 48656c6c6f..."
            buf.color_string();
            buf.append("\"<blob ");
            buf.append_uint(static_cast<uint64_t>(node->value.str.len));
            buf.append(" bytes>");
            // Show first few bytes inline.
            if (node->value.str.data && node->value.str.len > 0) {
                buf.append(": ");
                size_t preview = node->value.str.len;
                if (preview > 32) preview = 32;
                for (size_t i = 0; i < preview; ++i) {
                    buf.append_hex_byte(
                        static_cast<uint8_t>(node->value.str.data[i]));
                }
                if (node->value.str.len > 32) {
                    buf.append("...");
                }
            }
            buf.append_char('"');
            buf.color_reset();
            break;
        }

        case kTypeTimestamp:
            buf.color_number();
            buf.append_int(node->value.timestamp_val);
            buf.color_reset();
            break;

        case kTypeReference: {
            buf.color_type();
            buf.append("\"<ref #");
            buf.append_uint(static_cast<uint64_t>(node->value.ref_index));
            buf.append(">\"");
            buf.color_reset();
            break;
        }

        case kTypeCompressed:
            buf.color_type();
            buf.append("\"<compressed>\"");
            buf.color_reset();
            break;

        case kTypeArray:
        case kTypeTypedArray:
            json_emit_array(node, doc, buf, depth);
            break;

        case kTypeMap:
            json_emit_map(node, doc, buf, depth);
            break;

        default:
            buf.append("\"<unknown type ");
            buf.append_int(static_cast<int64_t>(node->type));
            buf.append(">\"");
            break;
    }
}

void PrettyPrinter::json_emit_array(const Node* node, const Document& doc,
                                     FormatBuffer& buf, int depth) const {
    uint32_t base, count;
    if (node->type == kTypeTypedArray) {
        base  = node->value.typed_array.first_child_idx;
        count = node->value.typed_array.count;
    } else {
        base  = node->value.container.first_child_idx;
        count = node->value.container.count;
    }

    if (count == 0) {
        buf.append("[]");
        return;
    }

    const Node* root = doc.root();
    if (!root) {
        buf.append("[]");
        return;
    }

    bool compact = json_should_compact(node, doc);

    if (compact) {
        buf.append("[ ");
        for (uint32_t i = 0; i < count; ++i) {
            if (static_cast<size_t>(base + i) >= doc.node_count()) break;
            const Node* child = root + (base + i);
            if (i > 0) buf.append(", ");
            json_emit_node(child, doc, buf, depth + 1);
        }
        buf.append(" ]");
    } else {
        buf.append_char('[');
        buf.newline();
        buf.indent();
        for (uint32_t i = 0; i < count; ++i) {
            if (static_cast<size_t>(base + i) >= doc.node_count()) break;
            const Node* child = root + (base + i);
            buf.write_indent();
            json_emit_node(child, doc, buf, depth + 1);
            if (i + 1 < count) {
                buf.append_char(',');
            }
            buf.newline();
        }
        buf.dedent();
        buf.write_indent();
        buf.append_char(']');
    }
}

void PrettyPrinter::json_emit_map(const Node* node, const Document& doc,
                                   FormatBuffer& buf, int depth) const {
    uint32_t base  = node->value.container.first_child_idx;
    uint32_t count = node->value.container.count;

    if (count == 0) {
        buf.append("{}");
        return;
    }

    const Node* root = doc.root();
    if (!root) {
        buf.append("{}");
        return;
    }

    bool compact = json_should_compact(node, doc);

    if (compact) {
        buf.append("{ ");
        for (uint32_t i = 0; i < count; ++i) {
            if (static_cast<size_t>(base + i) >= doc.node_count()) break;
            const Node* child = root + (base + i);
            if (i > 0) buf.append(", ");

            // Key
            const char* key = doc.key_name(child);
            buf.color_key();
            if (key) {
                append_escaped_string(buf, key, std::strlen(key));
            } else {
                buf.append("\"?\"");
            }
            buf.color_reset();

            if (cfg_.show_key_indices) {
                buf.color_type();
                buf.append("/*");
                buf.append_uint(static_cast<uint64_t>(child->key_string_index));
                buf.append("*/");
                buf.color_reset();
            }

            buf.append(": ");
            json_emit_node(child, doc, buf, depth + 1);
        }
        buf.append(" }");
    } else {
        buf.append_char('{');
        buf.newline();
        buf.indent();
        for (uint32_t i = 0; i < count; ++i) {
            if (static_cast<size_t>(base + i) >= doc.node_count()) break;
            const Node* child = root + (base + i);
            buf.write_indent();

            // Key
            const char* key = doc.key_name(child);
            buf.color_key();
            if (key) {
                append_escaped_string(buf, key, std::strlen(key));
            } else {
                buf.append("\"?\"");
            }
            buf.color_reset();

            if (cfg_.show_key_indices) {
                buf.color_type();
                buf.append(" /*");
                buf.append_uint(static_cast<uint64_t>(child->key_string_index));
                buf.append("*/");
                buf.color_reset();
            }

            buf.append(": ");
            json_emit_node(child, doc, buf, depth + 1);

            if (i + 1 < count) {
                buf.append_char(',');
            }
            buf.newline();
        }
        buf.dedent();
        buf.write_indent();
        buf.append_char('}');
    }
}

// ===================================================================
//  XML output
// ===================================================================
std::string PrettyPrinter::to_xml(const Document& doc) const {
    if (!doc.is_valid()) {
        return "<!-- invalid document -->\n";
    }
    const Node* root = doc.root();
    if (!root) {
        return "<!-- empty document -->\n";
    }

    FormatBuffer buf(cfg_);
    buf.append("<?xml version=\"1.0\" encoding=\"UTF-8\"?>");
    buf.newline();
    xml_emit_node(root, doc, buf, 0, "root");
    buf.newline();
    return buf.to_string();
}

void PrettyPrinter::xml_emit_node(const Node* node, const Document& doc,
                                   FormatBuffer& buf, int depth,
                                   const char* tag_override) const {
    if (!node) {
        buf.write_indent();
        buf.append("<null/>");
        return;
    }

    // Depth limit
    if (cfg_.max_depth >= 0 && depth > cfg_.max_depth) {
        buf.write_indent();
        buf.append("<!-- depth limit -->");
        return;
    }

    // Determine the tag name.  For map children, use the key; otherwise
    // fall back to the type name or the caller-provided override.
    const char* tag = tag_override;
    if (!tag || tag[0] == '\0') {
        const char* key = doc.key_name(node);
        tag = key ? key : type_name(node->type);
    }

    // Build an XML-safe tag name -- replace any non-alnum chars with '_'.
    char safe_tag[128];
    size_t tag_len = std::strlen(tag);
    if (tag_len >= sizeof(safe_tag)) tag_len = sizeof(safe_tag) - 1;
    for (size_t i = 0; i < tag_len; ++i) {
        char c = tag[i];
        safe_tag[i] = (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')
                      ? c : '_';
    }
    safe_tag[tag_len] = '\0';

    // Make sure it starts with a letter or underscore (XML rule).
    if (tag_len > 0 && std::isdigit(static_cast<unsigned char>(safe_tag[0]))) {
        // Prepend an underscore by shifting.
        if (tag_len + 1 < sizeof(safe_tag)) {
            std::memmove(safe_tag + 1, safe_tag, tag_len + 1);
            safe_tag[0] = '_';
            tag_len++;
        }
    }

    switch (node->type) {
        case kTypeNull:
            buf.write_indent();
            buf.append_char('<');
            buf.append(safe_tag);
            if (cfg_.show_type_annotations) {
                buf.append(" type=\"null\"");
            }
            buf.append("/>");
            break;

        case kTypeBool:
            buf.write_indent();
            buf.append_char('<');
            buf.append(safe_tag);
            if (cfg_.show_type_annotations) {
                buf.append(" type=\"bool\"");
            }
            buf.append_char('>');
            buf.append(node->value.bool_val ? "true" : "false");
            buf.append("</");
            buf.append(safe_tag);
            buf.append_char('>');
            break;

        case kTypeInt8:
        case kTypeInt16:
        case kTypeInt32:
        case kTypeInt64: {
            int64_t v = 0;
            const char* tname = type_name(node->type);
            switch (node->type) {
                case kTypeInt8:  v = node->value.i8_val;  break;
                case kTypeInt16: v = node->value.i16_val; break;
                case kTypeInt32: v = node->value.i32_val; break;
                case kTypeInt64: v = node->value.i64_val; break;
                default: break;
            }
            buf.write_indent();
            buf.append_char('<');
            buf.append(safe_tag);
            if (cfg_.show_type_annotations) {
                buf.append(" type=\"");
                buf.append(tname);
                buf.append_char('"');
            }
            buf.append_char('>');
            buf.append_int(v);
            buf.append("</");
            buf.append(safe_tag);
            buf.append_char('>');
            break;
        }

        case kTypeFloat32:
        case kTypeFloat64: {
            double v = (node->type == kTypeFloat32)
                       ? static_cast<double>(node->value.f32_val)
                       : node->value.f64_val;
            buf.write_indent();
            buf.append_char('<');
            buf.append(safe_tag);
            if (cfg_.show_type_annotations) {
                buf.append(" type=\"");
                buf.append(type_name(node->type));
                buf.append_char('"');
            }
            buf.append_char('>');
            buf.append_double(v);
            buf.append("</");
            buf.append(safe_tag);
            buf.append_char('>');
            break;
        }

        case kTypeString: {
            buf.write_indent();
            buf.append_char('<');
            buf.append(safe_tag);
            if (cfg_.show_type_annotations) {
                buf.append(" type=\"string\"");
            }
            buf.append_char('>');
            // XML-escape the string content (no surrounding quotes).
            if (node->value.str.data) {
                const char* s = node->value.str.data;
                size_t slen = node->value.str.len;
                if (cfg_.max_string_len > 0 && slen > cfg_.max_string_len) {
                    slen = cfg_.max_string_len;
                }
                for (size_t i = 0; i < slen; ++i) {
                    switch (s[i]) {
                        case '&':  buf.append("&amp;");  break;
                        case '<':  buf.append("&lt;");   break;
                        case '>':  buf.append("&gt;");   break;
                        case '"':  buf.append("&quot;"); break;
                        case '\'': buf.append("&apos;"); break;
                        default:
                            if (static_cast<unsigned char>(s[i]) < 0x20 && s[i] != '\n' && s[i] != '\r' && s[i] != '\t') {
                                // Skip non-printable control characters.
                            } else {
                                buf.append_char(s[i]);
                            }
                            break;
                    }
                }
                if (cfg_.max_string_len > 0 && node->value.str.len > cfg_.max_string_len) {
                    buf.append("...");
                }
            }
            buf.append("</");
            buf.append(safe_tag);
            buf.append_char('>');
            break;
        }

        case kTypeBlob: {
            buf.write_indent();
            buf.append_char('<');
            buf.append(safe_tag);
            buf.append(" type=\"blob\" size=\"");
            buf.append_uint(static_cast<uint64_t>(node->value.str.len));
            buf.append("\">");
            // Hex-encode the blob content.
            if (node->value.str.data) {
                size_t preview = node->value.str.len;
                if (preview > 64) preview = 64;
                for (size_t i = 0; i < preview; ++i) {
                    buf.append_hex_byte(
                        static_cast<uint8_t>(node->value.str.data[i]));
                }
                if (node->value.str.len > 64) {
                    buf.append("...");
                }
            }
            buf.append("</");
            buf.append(safe_tag);
            buf.append_char('>');
            break;
        }

        case kTypeTimestamp:
            buf.write_indent();
            buf.append_char('<');
            buf.append(safe_tag);
            if (cfg_.show_type_annotations) {
                buf.append(" type=\"timestamp\"");
            }
            buf.append_char('>');
            buf.append_int(node->value.timestamp_val);
            buf.append("</");
            buf.append(safe_tag);
            buf.append_char('>');
            break;

        case kTypeReference:
            buf.write_indent();
            buf.append_char('<');
            buf.append(safe_tag);
            buf.append(" type=\"reference\" ref=\"");
            buf.append_uint(static_cast<uint64_t>(node->value.ref_index));
            buf.append("\"/>");
            break;

        case kTypeCompressed:
            buf.write_indent();
            buf.append_char('<');
            buf.append(safe_tag);
            buf.append(" type=\"compressed\"/>");
            break;

        case kTypeArray:
        case kTypeTypedArray: {
            uint32_t b, cnt;
            if (node->type == kTypeTypedArray) {
                b   = node->value.typed_array.first_child_idx;
                cnt = node->value.typed_array.count;
            } else {
                b   = node->value.container.first_child_idx;
                cnt = node->value.container.count;
            }
            buf.write_indent();
            buf.append_char('<');
            buf.append(safe_tag);
            if (cfg_.show_type_annotations) {
                buf.append(" type=\"");
                buf.append(type_name(node->type));
                buf.append_char('"');
            }
            buf.append(" count=\"");
            buf.append_uint(static_cast<uint64_t>(cnt));
            buf.append("\">");
            buf.newline();
            buf.indent();

            const Node* r = doc.root();
            for (uint32_t i = 0; i < cnt && r; ++i) {
                if (static_cast<size_t>(b + i) >= doc.node_count()) break;
                const Node* child = r + (b + i);
                xml_emit_node(child, doc, buf, depth + 1, "item");
                buf.newline();
            }

            buf.dedent();
            buf.write_indent();
            buf.append("</");
            buf.append(safe_tag);
            buf.append_char('>');
            break;
        }

        case kTypeMap: {
            uint32_t b   = node->value.container.first_child_idx;
            uint32_t cnt = node->value.container.count;
            buf.write_indent();
            buf.append_char('<');
            buf.append(safe_tag);
            if (cfg_.show_type_annotations) {
                buf.append(" type=\"map\"");
            }
            buf.append_char('>');
            buf.newline();
            buf.indent();

            const Node* r = doc.root();
            for (uint32_t i = 0; i < cnt && r; ++i) {
                if (static_cast<size_t>(b + i) >= doc.node_count()) break;
                const Node* child = r + (b + i);
                // Use the child's key as the XML tag.
                const char* child_key = doc.key_name(child);
                xml_emit_node(child, doc, buf, depth + 1,
                              child_key ? child_key : "entry");
                buf.newline();
            }

            buf.dedent();
            buf.write_indent();
            buf.append("</");
            buf.append(safe_tag);
            buf.append_char('>');
            break;
        }

        default:
            buf.write_indent();
            buf.append("<unknown type=\"");
            buf.append_int(static_cast<int64_t>(node->type));
            buf.append("\"/>");
            break;
    }
}

// ===================================================================
//  YAML output
// ===================================================================
std::string PrettyPrinter::to_yaml(const Document& doc) const {
    if (!doc.is_valid()) {
        return "# invalid document\n";
    }
    const Node* root = doc.root();
    if (!root) {
        return "# empty document\n";
    }

    FormatBuffer buf(cfg_);
    buf.append("---");
    buf.newline();
    yaml_emit_node(root, doc, buf, 0, false);
    return buf.to_string();
}

void PrettyPrinter::yaml_emit_node(const Node* node, const Document& doc,
                                    FormatBuffer& buf, int depth,
                                    bool is_list_item) const {
    if (!node) {
        buf.append("null");
        buf.newline();
        return;
    }

    // Depth limit
    if (cfg_.max_depth >= 0 && depth > cfg_.max_depth) {
        buf.append("# ... depth limit ...");
        buf.newline();
        return;
    }

    switch (node->type) {
        case kTypeArray:
        case kTypeTypedArray:
            yaml_emit_array(node, doc, buf, depth);
            break;
        case kTypeMap:
            yaml_emit_map(node, doc, buf, depth);
            break;
        default:
            yaml_emit_value(node, doc, buf);
            buf.newline();
            break;
    }
}

void PrettyPrinter::yaml_emit_value(const Node* node, const Document& doc,
                                     FormatBuffer& buf) const {
    if (!node) {
        buf.append("null");
        return;
    }

    // Optional type annotation as a YAML comment
    if (cfg_.show_type_annotations) {
        buf.append(type_name(node->type));
        buf.append("!");
    }

    switch (node->type) {
        case kTypeNull:
            buf.append("null");
            break;

        case kTypeBool:
            buf.append(node->value.bool_val ? "true" : "false");
            break;

        case kTypeInt8:
            buf.append_int(static_cast<int64_t>(node->value.i8_val));
            break;

        case kTypeInt16:
            buf.append_int(static_cast<int64_t>(node->value.i16_val));
            break;

        case kTypeInt32:
            buf.append_int(static_cast<int64_t>(node->value.i32_val));
            break;

        case kTypeInt64:
            buf.append_int(node->value.i64_val);
            break;

        case kTypeFloat32:
            buf.append_double(static_cast<double>(node->value.f32_val));
            break;

        case kTypeFloat64:
            buf.append_double(node->value.f64_val);
            break;

        case kTypeString:
            if (node->value.str.data && node->value.str.len > 0) {
                // Check if the string needs quoting (contains special chars
                // or leading/trailing whitespace).
                const char* s = node->value.str.data;
                size_t slen = node->value.str.len;
                if (cfg_.max_string_len > 0 && slen > cfg_.max_string_len) {
                    slen = cfg_.max_string_len;
                }
                bool needs_quoting = false;
                if (slen > 0 && (s[0] == ' ' || s[slen - 1] == ' ')) {
                    needs_quoting = true;
                }
                for (size_t i = 0; !needs_quoting && i < slen; ++i) {
                    char c = s[i];
                    if (c == ':' || c == '#' || c == '\n' || c == '\r' ||
                        c == '"' || c == '\'' || c == '[' || c == ']' ||
                        c == '{' || c == '}' || c == ',' || c == '&' ||
                        c == '*' || c == '!' || c == '|' || c == '>' ||
                        c == '%' || c == '@') {
                        needs_quoting = true;
                    }
                }
                if (needs_quoting) {
                    append_truncated_string(buf, s, node->value.str.len);
                } else {
                    buf.append(s, slen);
                    if (cfg_.max_string_len > 0 && node->value.str.len > cfg_.max_string_len) {
                        buf.append("...");
                    }
                }
            } else {
                buf.append("\"\"");
            }
            break;

        case kTypeBlob:
            buf.append("<blob ");
            buf.append_uint(static_cast<uint64_t>(node->value.str.len));
            buf.append(" bytes>");
            break;

        case kTypeTimestamp:
            buf.append_int(node->value.timestamp_val);
            if (cfg_.show_type_annotations) {
                buf.append("  # timestamp");
            }
            break;

        case kTypeReference:
            buf.append("<ref #");
            buf.append_uint(static_cast<uint64_t>(node->value.ref_index));
            buf.append_char('>');
            break;

        case kTypeCompressed:
            buf.append("<compressed>");
            break;

        default:
            buf.append("<unknown>");
            break;
    }
}

void PrettyPrinter::yaml_emit_array(const Node* node, const Document& doc,
                                     FormatBuffer& buf, int depth) const {
    uint32_t base, count;
    if (node->type == kTypeTypedArray) {
        base  = node->value.typed_array.first_child_idx;
        count = node->value.typed_array.count;
    } else {
        base  = node->value.container.first_child_idx;
        count = node->value.container.count;
    }

    if (count == 0) {
        buf.append("[]");
        buf.newline();
        return;
    }

    const Node* root = doc.root();
    if (!root) {
        buf.append("[]");
        buf.newline();
        return;
    }

    // Check if we should use compact inline notation for small arrays
    // of scalars.
    if (cfg_.compact_small && count <= static_cast<uint32_t>(cfg_.compact_threshold)) {
        bool all_scalar = true;
        for (uint32_t i = 0; i < count && all_scalar; ++i) {
            if (static_cast<size_t>(base + i) >= doc.node_count()) break;
            const Node* child = root + (base + i);
            if (child->type == kTypeArray || child->type == kTypeMap ||
                child->type == kTypeTypedArray) {
                all_scalar = false;
            }
        }
        if (all_scalar) {
            buf.append("[");
            for (uint32_t i = 0; i < count; ++i) {
                if (static_cast<size_t>(base + i) >= doc.node_count()) break;
                const Node* child = root + (base + i);
                if (i > 0) buf.append(", ");
                yaml_emit_value(child, doc, buf);
            }
            buf.append("]");
            buf.newline();
            return;
        }
    }

    // Block-style YAML list.
    for (uint32_t i = 0; i < count; ++i) {
        if (static_cast<size_t>(base + i) >= doc.node_count()) break;
        const Node* child = root + (base + i);

        buf.write_indent();
        buf.append("- ");

        if (child->type == kTypeMap) {
            // Emit the first key-value on the same line as the dash, then
            // continue the rest indented.
            uint32_t mb = child->value.container.first_child_idx;
            uint32_t mc = child->value.container.count;
            if (mc > 0 && static_cast<size_t>(mb) < doc.node_count()) {
                const Node* first_kv = root + mb;
                const char* k = doc.key_name(first_kv);
                if (k) {
                    buf.append(k);
                } else {
                    buf.append("\"?\"");
                }
                buf.append(": ");
                if (first_kv->type == kTypeArray || first_kv->type == kTypeMap ||
                    first_kv->type == kTypeTypedArray) {
                    buf.newline();
                    buf.indent();
                    buf.indent();
                    yaml_emit_node(first_kv, doc, buf, depth + 2, true);
                    buf.dedent();
                    buf.dedent();
                } else {
                    yaml_emit_value(first_kv, doc, buf);
                    buf.newline();
                }
                // Remaining keys
                if (mc > 1) {
                    buf.indent();
                    for (uint32_t j = 1; j < mc; ++j) {
                        if (static_cast<size_t>(mb + j) >= doc.node_count()) break;
                        const Node* kv = root + (mb + j);
                        buf.write_indent();
                        const char* kk = doc.key_name(kv);
                        if (kk) {
                            buf.append(kk);
                        } else {
                            buf.append("\"?\"");
                        }
                        buf.append(": ");
                        if (kv->type == kTypeArray || kv->type == kTypeMap ||
                            kv->type == kTypeTypedArray) {
                            buf.newline();
                            buf.indent();
                            yaml_emit_node(kv, doc, buf, depth + 2, false);
                            buf.dedent();
                        } else {
                            yaml_emit_value(kv, doc, buf);
                            buf.newline();
                        }
                    }
                    buf.dedent();
                }
            } else {
                buf.append("{}");
                buf.newline();
            }
        } else if (child->type == kTypeArray || child->type == kTypeTypedArray) {
            buf.newline();
            buf.indent();
            yaml_emit_node(child, doc, buf, depth + 1, true);
            buf.dedent();
        } else {
            yaml_emit_value(child, doc, buf);
            buf.newline();
        }
    }
}

void PrettyPrinter::yaml_emit_map(const Node* node, const Document& doc,
                                   FormatBuffer& buf, int depth) const {
    uint32_t base  = node->value.container.first_child_idx;
    uint32_t count = node->value.container.count;

    if (count == 0) {
        buf.append("{}");
        buf.newline();
        return;
    }

    const Node* root = doc.root();
    if (!root) {
        buf.append("{}");
        buf.newline();
        return;
    }

    for (uint32_t i = 0; i < count; ++i) {
        if (static_cast<size_t>(base + i) >= doc.node_count()) break;
        const Node* child = root + (base + i);

        buf.write_indent();
        const char* key = doc.key_name(child);
        if (key) {
            buf.append(key);
        } else {
            buf.append("\"?\"");
        }

        if (cfg_.show_key_indices) {
            buf.append("  # idx=");
            buf.append_uint(static_cast<uint64_t>(child->key_string_index));
        }

        buf.append(": ");

        if (child->type == kTypeArray || child->type == kTypeMap ||
            child->type == kTypeTypedArray) {
            buf.newline();
            buf.indent();
            yaml_emit_node(child, doc, buf, depth + 1, false);
            buf.dedent();
        } else {
            yaml_emit_value(child, doc, buf);
            buf.newline();
        }
    }
}

}  // namespace helios
