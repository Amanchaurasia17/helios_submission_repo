#ifndef HELIOS_PARSER_H_
#define HELIOS_PARSER_H_

#include "helios_types.h"
#include "helios_node.h"
#include "helios_string_table.h"
#include <cstdint>
#include <cstddef>
#include <cstdarg>

namespace helios {

class Parser {
public:
    Parser();
    ~Parser();

    // Main entry point. Returns kOk on success.
    ErrorCode parse(const uint8_t* data, size_t len);

    // Results (only valid after parse() returns kOk)
    const Node*        root()         const;
    size_t             node_count()   const;
    const NodePool&    pool()         const;
    const StringTable& string_table() const;

    // Error info
    const char* error_message() const;
    ErrorCode   error_code()    const;

private:
    // --- parsing pipeline ---
    bool read_header();
    bool validate_footer();
    bool ensure_string_table();
    Node* parse_element();
    Node* parse_scalar(ElementType type);
    Node* parse_string_element();
    Node* parse_blob_element();
    Node* parse_array_element();
    Node* parse_map_element();
    Node* parse_compressed_element();
    Node* parse_typed_array_element();
    Node* resolve_reference(uint32_t ref_index);

    // --- low-level helpers ---
    bool has_bytes(size_t n) const;
    void set_error(ErrorCode code, const char* fmt, ...);

    // --- state ---
    const uint8_t* data_;
    size_t         data_len_;
    size_t         pos_;
    int            depth_;
    uint32_t       root_node_index_;
    FileHeader     header_;
    NodePool       pool_;
    StringTable    string_table_;
    bool           string_table_loaded_;
    ErrorCode      error_code_;
    char           error_msg_[512];
};

}  // namespace helios
#endif  // HELIOS_PARSER_H_
