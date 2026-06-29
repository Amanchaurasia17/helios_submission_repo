#ifndef HELIOS_SCHEMA_H_
#define HELIOS_SCHEMA_H_

#include "helios_node.h"
#include <string>
#include <vector>
#include <map>
#include <memory>

namespace helios {
namespace schema {

enum class SchemaType {
    Any,
    Null,
    Boolean,
    Integer,
    Float,
    String,
    Blob,
    Array,
    Map,
    Timestamp
};

class SchemaValidator;

class SchemaRule {
public:
    virtual ~SchemaRule() = default;
    virtual bool validate(const Node* node, std::string& error_msg) const = 0;
};

class TypeRule : public SchemaRule {
    SchemaType expected_type_;
public:
    explicit TypeRule(SchemaType t) : expected_type_(t) {}
    bool validate(const Node* node, std::string& error_msg) const override;
};

class RangeRule : public SchemaRule {
    double min_val_;
    double max_val_;
public:
    RangeRule(double min_val, double max_val) : min_val_(min_val), max_val_(max_val) {}
    bool validate(const Node* node, std::string& error_msg) const override;
};

class StringLengthRule : public SchemaRule {
    size_t min_len_;
    size_t max_len_;
public:
    StringLengthRule(size_t min_len, size_t max_len) : min_len_(min_len), max_len_(max_len) {}
    bool validate(const Node* node, std::string& error_msg) const override;
};

class ArraySchemaRule : public SchemaRule {
    std::shared_ptr<SchemaValidator> item_validator_;
    size_t min_items_;
    size_t max_items_;
public:
    ArraySchemaRule(std::shared_ptr<SchemaValidator> item_validator, size_t min_items, size_t max_items)
        : item_validator_(item_validator), min_items_(min_items), max_items_(max_items) {}
    bool validate(const Node* node, std::string& error_msg) const override;
};

class SchemaValidator {
    std::vector<std::shared_ptr<SchemaRule>> rules_;
public:
    void add_rule(std::shared_ptr<SchemaRule> rule) { rules_.push_back(rule); }
    bool validate(const Node* node, std::string& error_msg) const;
};

} // namespace schema
} // namespace helios

#endif // HELIOS_SCHEMA_H_
