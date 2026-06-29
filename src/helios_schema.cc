#include "helios_schema.h"
#include <sstream>
#include <algorithm>

namespace helios {
namespace schema {

bool TypeRule::validate(const Node* node, std::string& error_msg) const {
    if (!node) {
        error_msg = "Node is null";
        return false;
    }
    bool match = false;
    switch (expected_type_) {
        case SchemaType::Any: match = true; break;
        case SchemaType::Null: match = (node->type == kTypeNull); break;
        case SchemaType::Boolean: match = (node->type == kTypeBool); break;
        case SchemaType::Integer:
            match = (node->type == kTypeInt8 || node->type == kTypeInt16 ||
                     node->type == kTypeInt32 || node->type == kTypeInt64);
            break;
        case SchemaType::Float:
            match = (node->type == kTypeFloat32 || node->type == kTypeFloat64);
            break;
        case SchemaType::String: match = (node->type == kTypeString); break;
        case SchemaType::Blob: match = (node->type == kTypeBlob); break;
        case SchemaType::Array: match = (node->type == kTypeArray || node->type == kTypeTypedArray); break;
        case SchemaType::Map: match = (node->type == kTypeMap); break;
        case SchemaType::Timestamp: match = (node->type == kTypeTimestamp); break;
    }
    if (!match) {
        error_msg = "Type mismatch: expected " + std::to_string(static_cast<int>(expected_type_));
        return false;
    }
    return true;
}

bool RangeRule::validate(const Node* node, std::string& error_msg) const {
    if (!node) { error_msg = "null node"; return false; }
    double val = 0;
    switch (node->type) {
        case kTypeInt8:    val = node->value.i8_val;  break;
        case kTypeInt16:   val = node->value.i16_val; break;
        case kTypeInt32:   val = node->value.i32_val; break;
        case kTypeInt64:   val = static_cast<double>(node->value.i64_val); break;
        case kTypeFloat32: val = node->value.f32_val; break;
        case kTypeFloat64: val = node->value.f64_val; break;
        default:
            error_msg = "Node is not numeric";
            return false;
    }
    if (val < min_val_ || val > max_val_) {
        std::ostringstream oss;
        oss << "Value " << val << " out of range [" << min_val_ << ", " << max_val_ << "]";
        error_msg = oss.str();
        return false;
    }
    return true;
}

bool StringLengthRule::validate(const Node* node, std::string& error_msg) const {
    if (!node || node->type != kTypeString) {
        error_msg = "Node is not a string";
        return false;
    }
    size_t len = node->value.str.len;
    if (len < min_len_ || len > max_len_) {
        std::ostringstream oss;
        oss << "String length " << len << " out of bounds [" << min_len_ << ", " << max_len_ << "]";
        error_msg = oss.str();
        return false;
    }
    return true;
}

bool ArraySchemaRule::validate(const Node* node, std::string& error_msg) const {
    if (!node || (node->type != kTypeArray && node->type != kTypeTypedArray)) {
        error_msg = "Node is not an array";
        return false;
    }
    size_t count = node->value.container.count;
    if (count < min_items_ || count > max_items_) {
        std::ostringstream oss;
        oss << "Array count " << count << " out of bounds [" << min_items_ << ", " << max_items_ << "]";
        error_msg = oss.str();
        return false;
    }
    return true;
}

bool SchemaValidator::validate(const Node* node, std::string& error_msg) const {
    for (const auto& rule : rules_) {
        if (!rule->validate(node, error_msg)) {
            return false;
        }
    }
    return true;
}

}  // namespace schema
}  // namespace helios
