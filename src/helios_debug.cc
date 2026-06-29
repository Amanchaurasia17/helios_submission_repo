// src/helios_debug.cc
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cstdio>
#include "helios/helios_debug.h"

namespace helios {

const char* element_type_to_string(ElementType type) {
    switch (type) {
        case kTypeNull:       return "kTypeNull";
        case kTypeBool:       return "kTypeBool";
        case kTypeInt8:       return "kTypeInt8";
        case kTypeInt16:      return "kTypeInt16";
        case kTypeInt32:      return "kTypeInt32";
        case kTypeInt64:      return "kTypeInt64";
        case kTypeFloat32:    return "kTypeFloat32";
        case kTypeFloat64:    return "kTypeFloat64";
        case kTypeString:     return "kTypeString";
        case kTypeBlob:       return "kTypeBlob";
        case kTypeArray:      return "kTypeArray";
        case kTypeMap:        return "kTypeMap";
        case kTypeCompressed: return "kTypeCompressed";
        case kTypeReference:  return "kTypeReference";
        case kTypeTimestamp:  return "kTypeTimestamp";
        case kTypeTypedArray: return "kTypeTypedArray";
        default:              return "kTypeUnknown";
    }
}

std::string format_node_value(const Node* node, const StringTable& string_table) {
    if (!node) return "nullptr";

    std::stringstream ss;
    switch (node->type) {
        case kTypeNull:
            return "null";
            
        case kTypeBool:
            return node->value.bool_val ? "true" : "false";
            
        case kTypeInt8:
            ss << static_cast<int>(node->value.i8_val);
            break;
            
        case kTypeInt16:
            ss << node->value.i16_val;
            break;
            
        case kTypeInt32:
            ss << node->value.i32_val;
            break;
            
        case kTypeInt64:
            ss << node->value.i64_val;
            break;
            
        case kTypeFloat32:
            ss << std::fixed << std::setprecision(6) << node->value.f32_val;
            break;
            
        case kTypeFloat64:
            ss << std::fixed << std::setprecision(10) << node->value.f64_val;
            break;
            
        case kTypeString: {
            if (node->value.str.data) {
                ss << "\"" << node->value.str.data << "\"";
            } else {
                ss << "\"\" (empty)";
            }
            break;
        }
        
        case kTypeBlob: {
            ss << "blob(" << node->value.str.len << " bytes): [";
            size_t preview_len = std::min(node->value.str.len, static_cast<size_t>(8));
            for (size_t i = 0; i < preview_len; ++i) {
                ss << std::hex << std::setw(2) << std::setfill('0')
                   << static_cast<int>(static_cast<unsigned char>(node->value.str.data[i]));
                if (i + 1 < preview_len) ss << " ";
            }
            if (node->value.str.len > 8) ss << " ...";
            ss << "]";
            break;
        }
        
        case kTypeArray:
            ss << "array(count: " << node->value.container.count << ")";
            break;
            
        case kTypeMap:
            ss << "map(count: " << node->value.container.count << ")";
            break;
            
        case kTypeCompressed:
            ss << "compressed(subroot)";
            break;
            
        case kTypeReference:
            ss << "reference(target_idx: " << node->value.ref_index << ")";
            break;
            
        case kTypeTimestamp:
            ss << "timestamp(" << node->value.timestamp_val << ")";
            break;
            
        case kTypeTypedArray:
            ss << "typed_array(type: " << element_type_to_string(node->value.typed_array.elem_type)
               << ", count: " << node->value.typed_array.count << ")";
            break;
            
        default:
            ss << "invalid_value";
            break;
    }
    return ss.str();
}

std::string dump_node_tree(const Node* node, const NodePool& pool, const StringTable& string_table, int indent_level) {
    if (!node) return "";

    std::stringstream ss;
    std::string indent(indent_level * 2, ' ');
    
    // Resolve key if this node is in a Map
    std::string key_prefix = "";
    if (node->key_string_index != 0xFFFFFFFF) {
        size_t key_len = 0;
        const char* key = string_table.get_string(node->key_string_index, &key_len);
        if (key) {
            key_prefix = "\"" + std::string(key, key_len) + "\": ";
        } else {
            key_prefix = "<corrupt_key>: ";
        }
    }

    ss << indent << "├── " << key_prefix << element_type_to_string(node->type)
       << " -> " << format_node_value(node, string_table)
       << " [offset: " << node->field_offset << "]\n";

    if (node->type == kTypeArray || node->type == kTypeMap) {
        uint32_t first = node->value.container.first_child_idx;
        uint32_t count = node->value.container.count;
        for (uint32_t i = 0; i < count; ++i) {
            const Node* child = pool.get(first + i);
            if (child) {
                ss << dump_node_tree(child, pool, string_table, indent_level + 1);
            }
        }
    } else if (node->type == kTypeTypedArray) {
        uint32_t first = node->value.typed_array.first_child_idx;
        uint32_t count = node->value.typed_array.count;
        for (uint32_t i = 0; i < count; ++i) {
            const Node* child = pool.get(first + i);
            if (child) {
                ss << dump_node_tree(child, pool, string_table, indent_level + 1);
            }
        }
    }

    return ss.str();
}

std::string serialize_node_to_json(const Node* node, const NodePool& pool, const StringTable& string_table, bool pretty_print, int indent_level) {
    if (!node) return "null";

    std::string newline = pretty_print ? "\n" : "";
    std::string space = pretty_print ? " " : "";
    std::string indent(indent_level * 2, ' ');
    std::string child_indent((indent_level + 1) * 2, ' ');

    std::stringstream ss;

    switch (node->type) {
        case kTypeNull:
            return "null";
            
        case kTypeBool:
            return node->value.bool_val ? "true" : "false";
            
        case kTypeInt8:
            ss << static_cast<int>(node->value.i8_val);
            return ss.str();
            
        case kTypeInt16:
            ss << node->value.i16_val;
            return ss.str();
            
        case kTypeInt32:
            ss << node->value.i32_val;
            return ss.str();
            
        case kTypeInt64:
            ss << node->value.i64_val;
            return ss.str();
            
        case kTypeFloat32:
            ss << node->value.f32_val;
            return ss.str();
            
        case kTypeFloat64:
            ss << node->value.f64_val;
            return ss.str();
            
        case kTypeString:
            if (node->value.str.data) {
                return "\"" + std::string(node->value.str.data) + "\"";
            }
            return "\"\"";
            
        case kTypeBlob:
            // JSON represents blobs as hex strings
            ss << "\"0x";
            for (size_t i = 0; i < node->value.str.len; ++i) {
                ss << std::hex << std::setw(2) << std::setfill('0')
                   << static_cast<int>(static_cast<unsigned char>(node->value.str.data[i]));
            }
            ss << "\"";
            return ss.str();
            
        case kTypeTimestamp:
            ss << node->value.timestamp_val;
            return ss.str();

        case kTypeReference:
            // JSON representation of reference points to index
            ss << "{\"_ref_target\":" << space << node->value.ref_index << "}";
            return ss.str();

        case kTypeCompressed:
            // Just JSON serialize the inner root directly (if it has fields)
            return "{\"_compressed\":true}";

        case kTypeArray: {
            uint32_t first = node->value.container.first_child_idx;
            uint32_t count = node->value.container.count;
            if (count == 0) return "[]";
            
            ss << "[" << newline;
            for (uint32_t i = 0; i < count; ++i) {
                const Node* child = pool.get(first + i);
                ss << child_indent << serialize_node_to_json(child, pool, string_table, pretty_print, indent_level + 1);
                if (i + 1 < count) ss << ",";
                ss << newline;
            }
            ss << indent << "]";
            return ss.str();
        }

        case kTypeTypedArray: {
            uint32_t first = node->value.typed_array.first_child_idx;
            uint32_t count = node->value.typed_array.count;
            if (count == 0) return "[]";
            
            ss << "[" << newline;
            for (uint32_t i = 0; i < count; ++i) {
                const Node* child = pool.get(first + i);
                ss << child_indent << serialize_node_to_json(child, pool, string_table, pretty_print, indent_level + 1);
                if (i + 1 < count) ss << ",";
                ss << newline;
            }
            ss << indent << "]";
            return ss.str();
        }

        case kTypeMap: {
            uint32_t first = node->value.container.first_child_idx;
            uint32_t count = node->value.container.count;
            if (count == 0) return "{}";
            
            ss << "{" << newline;
            for (uint32_t i = 0; i < count; ++i) {
                const Node* child = pool.get(first + i);
                if (!child) continue;
                
                std::string key_str = "key_" + std::to_string(i);
                if (child->key_string_index != 0xFFFFFFFF) {
                    size_t key_len = 0;
                    const char* key = string_table.get_string(child->key_string_index, &key_len);
                    if (key) {
                        key_str = std::string(key, key_len);
                    }
                }
                
                ss << child_indent << "\"" << key_str << "\":" << space
                   << serialize_node_to_json(child, pool, string_table, pretty_print, indent_level + 1);
                
                if (i + 1 < count) ss << ",";
                ss << newline;
            }
            ss << indent << "}";
            return ss.str();
        }
        
        default:
            return "\"invalid\"";
    }
}

DocumentInspector::DocumentInspector(const Node* root, const NodePool& pool)
    : root_(root), pool_(pool), total_nodes_(0), max_depth_(0), analyzed_(false) {
    std::fill(std::begin(type_counts_), std::end(type_counts_), 0);
}

void DocumentInspector::analyze() {
    if (analyzed_ || !root_) return;
    
    traverse(root_, 1);
    analyzed_ = true;
}

void DocumentInspector::traverse(const Node* node, int current_depth) {
    if (!node) return;
    
    total_nodes_++;
    type_counts_[static_cast<uint8_t>(node->type)]++;
    max_depth_ = std::max(max_depth_, current_depth);
    
    if (node->type == kTypeArray || node->type == kTypeMap) {
        uint32_t first = node->value.container.first_child_idx;
        uint32_t count = node->value.container.count;
        for (uint32_t i = 0; i < count; ++i) {
            traverse(pool_.get(first + i), current_depth + 1);
        }
    } else if (node->type == kTypeTypedArray) {
        uint32_t first = node->value.typed_array.first_child_idx;
        uint32_t count = node->value.typed_array.count;
        for (uint32_t i = 0; i < count; ++i) {
            traverse(pool_.get(first + i), current_depth + 1);
        }
    }
}

size_t DocumentInspector::get_total_nodes() const { return total_nodes_; }

size_t DocumentInspector::get_type_count(ElementType type) const {
    return type_counts_[static_cast<uint8_t>(type)];
}

int DocumentInspector::get_max_depth() const { return max_depth_; }

std::string DocumentInspector::generate_report(const StringTable& string_table) const {
    if (!analyzed_) return "Document not analyzed yet. Run analyze() first.";

    std::stringstream ss;
    ss << "=== Helios Document Inspection Report ===\n"
       << "Total parsed nodes: " << total_nodes_ << "\n"
       << "Maximum nesting depth: " << max_depth_ << "\n"
       << "--- Type Frequencies ---\n";
       
    for (int i = 0; i < 256; ++i) {
        if (type_counts_[i] > 0) {
            ElementType t = static_cast<ElementType>(i);
            ss << " - " << element_type_to_string(t) << ": " << type_counts_[i] << "\n";
        }
    }
    
    if (string_table.is_loaded()) {
        ss << "--- String Table Statistics ---\n"
           << "Loaded string entries: " << string_table.count() << "\n";
    } else {
        ss << "String Table: Not loaded/None\n";
    }
    ss << "========================================\n";
    
    return ss.str();
}

} // namespace helios
