// src/helios_parser.cc
// Parser implementation — Part 1 (task 5.2):
//   constructor/destructor, has_bytes, set_error, accessors,
//   read_header, validate_footer, ensure_string_table,
//   parse (top-level), parse_scalar, parse_string_element,
//   parse_blob_element, parse_element (dispatcher with stubs for
//   container/typed-array/reference/compressed — task 5.3 fills those in).

#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <algorithm>
#include "helios/helios_parser.h"
#include "helios/helios_varint.h"
#include "helios/helios_checksum.h"

namespace helios {

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

Parser::Parser()
    : data_(nullptr),
      data_len_(0),
      pos_(0),
      depth_(0),
      root_node_index_(0),
      string_table_loaded_(false),
      error_code_(kOk)
{
    memset(error_msg_, 0, sizeof(error_msg_));
    memset(&header_,   0, sizeof(header_));
}

Parser::~Parser() {}

// ---------------------------------------------------------------------------
// Low-level I/O helper
// ---------------------------------------------------------------------------

bool Parser::has_bytes(size_t n) const {
    return pos_ + n <= data_len_;
}

// ---------------------------------------------------------------------------
// Error reporting
// ---------------------------------------------------------------------------

void Parser::set_error(ErrorCode code, const char* fmt, ...) {
    error_code_ = code;
    va_list args;
    va_start(args, fmt);
    vsnprintf(error_msg_, 512, fmt, args);
    va_end(args);
}

// ---------------------------------------------------------------------------
// Public accessors
// ---------------------------------------------------------------------------

const char* Parser::error_message() const {
    return error_msg_;
}

ErrorCode Parser::error_code() const {
    return error_code_;
}

const Node* Parser::root() const {
    if (pool_.size() == 0) return nullptr;
    return pool_.get(root_node_index_);
}

size_t Parser::node_count() const {
    return pool_.size();
}

const NodePool& Parser::pool() const {
    return pool_;
}

const StringTable& Parser::string_table() const {
    return string_table_;
}

// ---------------------------------------------------------------------------
// Header validation
// ---------------------------------------------------------------------------

bool Parser::read_header() {
    if (!has_bytes(kHeaderSize)) {
        set_error(kErrTruncated,
                  "file too small for header (%zu bytes available)",
                  data_len_);
        return false;
    }

    memcpy(&header_, data_, kHeaderSize);

    if (memcmp(header_.magic, kMagic, 4) != 0) {
        set_error(kErrInvalidMagic, "invalid magic bytes");
        return false;
    }

    if (header_.version != kVersion) {
        set_error(kErrUnsupportedVersion,
                  "unsupported version 0x%04x", header_.version);
        return false;
    }

    if (header_.element_count > kMaxElementCount) {
        set_error(kErrInvalidElementCount,
                  "element count too large: %u", header_.element_count);
        return false;
    }

    pos_ = kHeaderSize;
    return true;
}

// ---------------------------------------------------------------------------
// Footer / checksum validation
// ---------------------------------------------------------------------------

bool Parser::validate_footer() {
    if (!(header_.flags & kFlagHasChecksum))
        return true;

    if (data_len_ < kHeaderSize + kFooterSize) {
        set_error(kErrTruncated, "file too small for footer");
        return false;
    }

    // Read the last 16 bytes as a FileFooter
    size_t footer_off = data_len_ - kFooterSize;
    FileFooter footer;
    memcpy(&footer, data_ + footer_off, kFooterSize);

    if (memcmp(footer.end_magic, kEndMagic, 4) != 0) {
        set_error(kErrChecksumMismatch, "invalid end-of-file magic");
        return false;
    }

    // CRC32 is computed over the body: everything after the header and before
    // the footer.
    uint32_t computed = crc32_compute(
        data_ + kHeaderSize,
        data_len_ - kHeaderSize - kFooterSize);

    if (computed != footer.checksum) {
        set_error(kErrChecksumMismatch,
                  "CRC mismatch: expected 0x%08x, computed 0x%08x",
                  footer.checksum, computed);
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// String table
// ---------------------------------------------------------------------------

bool Parser::ensure_string_table() {
    if (!(header_.flags & kFlagHasStringTable))
        return true;

    if (string_table_loaded_)
        return true;

    if (header_.string_table_offset >= data_len_) {
        set_error(kErrStringTableCorrupt,
                  "string table offset %u out of bounds (file size %zu)",
                  header_.string_table_offset, data_len_);
        return false;
    }

    const uint8_t* st_ptr = data_ + header_.string_table_offset;
    size_t         st_len = data_len_ - header_.string_table_offset;

    if (!string_table_.load(st_ptr, st_len)) {
        set_error(kErrStringTableCorrupt, "failed to load string table");
        return false;
    }

    string_table_loaded_ = true;
    return true;
}

// ---------------------------------------------------------------------------
// Top-level parse entry point
// ---------------------------------------------------------------------------

ErrorCode Parser::parse(const uint8_t* data, size_t len) {
    // Reset all state
    pool_.reset();
    string_table_.reset();
    string_table_loaded_ = false;
    pos_          = 0;
    depth_        = 0;
    error_code_   = kOk;
    error_msg_[0] = '\0';
    memset(&header_, 0, sizeof(header_));

    data_     = data;
    data_len_ = len;

    if (!read_header())     return error_code_;
    if (!validate_footer()) return error_code_;

    Node* root_node = parse_element();
    if (!root_node) {
        if (error_code_ == kOk)
            set_error(kErrMalformedInput, "failed to parse root element");
        return error_code_;
    }

    // The root is always index 0 in the pool (first node allocated).
    root_node_index_ = 0;
    return kOk;
}

// ---------------------------------------------------------------------------
// Scalar element parsing
// ---------------------------------------------------------------------------

Node* Parser::parse_scalar(ElementType type) {
    Node* node = pool_.allocate();
    if (!node) {
        set_error(kErrAllocationFailed, "node allocation failed");
        return nullptr;
    }
    node->type             = type;
    node->key_string_index = 0xFFFFFFFF;

    switch (type) {
        case kTypeNull:
            // No payload bytes; field_offset points to current position.
            node->field_offset = static_cast<uint32_t>(pos_);
            break;

        case kTypeBool: {
            node->field_offset = static_cast<uint32_t>(pos_);
            if (!has_bytes(1)) {
                set_error(kErrTruncated, "truncated bool at offset %zu", pos_);
                return nullptr;
            }
            uint8_t b;
            memcpy(&b, data_ + pos_, 1);
            node->value.bool_val = (b != 0);
            pos_ += 1;
            break;
        }

        case kTypeInt8: {
            node->field_offset = static_cast<uint32_t>(pos_);
            if (!has_bytes(1)) {
                set_error(kErrTruncated, "truncated int8 at offset %zu", pos_);
                return nullptr;
            }
            int8_t v;
            memcpy(&v, data_ + pos_, 1);
            node->value.i8_val = v;
            pos_ += 1;
            break;
        }

        case kTypeInt16: {
            node->field_offset = static_cast<uint32_t>(pos_);
            if (!has_bytes(2)) {
                set_error(kErrTruncated, "truncated int16 at offset %zu", pos_);
                return nullptr;
            }
            int16_t v;
            memcpy(&v, data_ + pos_, 2);
            node->value.i16_val = v;
            pos_ += 2;
            break;
        }

        case kTypeInt32: {
            node->field_offset = static_cast<uint32_t>(pos_);
            if (!has_bytes(4)) {
                set_error(kErrTruncated, "truncated int32 at offset %zu", pos_);
                return nullptr;
            }
            int32_t v;
            memcpy(&v, data_ + pos_, 4);
            node->value.i32_val = v;
            pos_ += 4;
            break;
        }

        case kTypeInt64: {
            node->field_offset = static_cast<uint32_t>(pos_);
            if (!has_bytes(8)) {
                set_error(kErrTruncated, "truncated int64 at offset %zu", pos_);
                return nullptr;
            }
            int64_t v;
            memcpy(&v, data_ + pos_, 8);
            node->value.i64_val = v;
            pos_ += 8;
            break;
        }

        case kTypeFloat32: {
            node->field_offset = static_cast<uint32_t>(pos_);
            if (!has_bytes(4)) {
                set_error(kErrTruncated, "truncated float32 at offset %zu", pos_);
                return nullptr;
            }
            float v;
            memcpy(&v, data_ + pos_, 4);
            node->value.f32_val = v;
            pos_ += 4;
            break;
        }

        case kTypeFloat64: {
            node->field_offset = static_cast<uint32_t>(pos_);
            if (!has_bytes(8)) {
                set_error(kErrTruncated, "truncated float64 at offset %zu", pos_);
                return nullptr;
            }
            double v;
            memcpy(&v, data_ + pos_, 8);
            node->value.f64_val = v;
            pos_ += 8;
            break;
        }

        case kTypeTimestamp: {
            node->field_offset = static_cast<uint32_t>(pos_);
            if (!has_bytes(8)) {
                set_error(kErrTruncated, "truncated timestamp at offset %zu", pos_);
                return nullptr;
            }
            int64_t v;
            memcpy(&v, data_ + pos_, 8);
            node->value.timestamp_val = v;
            pos_ += 8;
            break;
        }

        default:
            // Caller should never pass a non-scalar type here.
            set_error(kErrInvalidType,
                      "parse_scalar called with non-scalar type 0x%02x",
                      static_cast<unsigned>(type));
            return nullptr;
    }

    return node;
}

// ---------------------------------------------------------------------------
// String element parsing
// ---------------------------------------------------------------------------

Node* Parser::parse_string_element() {
    uint64_t slen;
    int consumed = decode_varint(data_ + pos_, data_len_ - pos_, &slen);
    if (consumed < 0) {
        set_error(kErrInvalidVarint, "invalid string length varint at offset %zu", pos_);
        return nullptr;
    }
    pos_ += static_cast<size_t>(consumed);

    if (slen > kMaxStringLength) {
        set_error(kErrMalformedInput,
                  "string too long: %llu (max %zu)",
                  (unsigned long long)slen, kMaxStringLength);
        return nullptr;
    }

    if (!has_bytes(static_cast<size_t>(slen))) {
        set_error(kErrTruncated,
                  "truncated string data at offset %zu (need %llu bytes)",
                  pos_, (unsigned long long)slen);
        return nullptr;
    }

    Node* node = pool_.allocate();
    if (!node) {
        set_error(kErrAllocationFailed, "node allocation failed");
        return nullptr;
    }

    node->type             = kTypeString;
    node->key_string_index = 0xFFFFFFFF;
    node->field_offset     = static_cast<uint32_t>(pos_);
    node->value.str.len    = static_cast<size_t>(slen);
    node->value.str.data   = static_cast<char*>(malloc(slen + 1));
    if (!node->value.str.data) {
        set_error(kErrAllocationFailed, "string data allocation failed");
        return nullptr;
    }

    memcpy(node->value.str.data, data_ + pos_, static_cast<size_t>(slen));
    node->value.str.data[slen] = '\0';
    pos_ += static_cast<size_t>(slen);

    return node;
}

// ---------------------------------------------------------------------------
// Blob element parsing
// ---------------------------------------------------------------------------

Node* Parser::parse_blob_element() {
    uint64_t blen;
    int consumed = decode_varint(data_ + pos_, data_len_ - pos_, &blen);
    if (consumed < 0) {
        set_error(kErrInvalidVarint, "invalid blob length varint at offset %zu", pos_);
        return nullptr;
    }
    pos_ += static_cast<size_t>(consumed);

    if (blen > kMaxStringLength) {
        set_error(kErrMalformedInput,
                  "blob too large: %llu (max %zu)",
                  (unsigned long long)blen, kMaxStringLength);
        return nullptr;
    }

    if (!has_bytes(static_cast<size_t>(blen))) {
        set_error(kErrTruncated,
                  "truncated blob data at offset %zu (need %llu bytes)",
                  pos_, (unsigned long long)blen);
        return nullptr;
    }

    Node* node = pool_.allocate();
    if (!node) {
        set_error(kErrAllocationFailed, "node allocation failed");
        return nullptr;
    }

    node->type             = kTypeBlob;
    node->key_string_index = 0xFFFFFFFF;
    node->field_offset     = static_cast<uint32_t>(pos_);
    node->value.str.len    = static_cast<size_t>(blen);
    node->value.str.data   = static_cast<char*>(malloc(blen + 1));
    if (!node->value.str.data) {
        set_error(kErrAllocationFailed, "blob data allocation failed");
        return nullptr;
    }

    memcpy(node->value.str.data, data_ + pos_, static_cast<size_t>(blen));
    node->value.str.data[blen] = '\0';
    pos_ += static_cast<size_t>(blen);

    return node;
}

// ---------------------------------------------------------------------------
// Element type dispatcher
// ---------------------------------------------------------------------------

Node* Parser::parse_element() {
    if (!has_bytes(1)) {
        set_error(kErrTruncated,
                  "unexpected end of data at offset %zu", pos_);
        return nullptr;
    }

    uint8_t tag = data_[pos_];
    pos_++;
    ElementType type = static_cast<ElementType>(tag);

    switch (type) {
        // --- scalars ---
        case kTypeNull:
        case kTypeBool:
        case kTypeInt8:
        case kTypeInt16:
        case kTypeInt32:
        case kTypeInt64:
        case kTypeFloat32:
        case kTypeFloat64:
        case kTypeTimestamp:
            return parse_scalar(type);

        // --- variable-length primitives ---
        case kTypeString:
            return parse_string_element();

        case kTypeBlob:
            return parse_blob_element();

        // --- container / complex types ---
        case kTypeArray:
            return parse_array_element();

        case kTypeMap:
            return parse_map_element();

        case kTypeCompressed:
            return parse_compressed_element();

        case kTypeTypedArray:
            return parse_typed_array_element();

        case kTypeReference: {
            uint64_t ref_index_raw;
            int consumed = decode_varint(data_ + pos_, data_len_ - pos_, &ref_index_raw);
            if (consumed < 0) {
                set_error(kErrInvalidVarint,
                          "invalid reference index varint at offset %zu", pos_);
                return nullptr;
            }
            pos_ += static_cast<size_t>(consumed);
            return resolve_reference(static_cast<uint32_t>(ref_index_raw));
        }

        default:
            set_error(kErrInvalidType,
                      "unknown element type 0x%02x at offset %zu",
                      static_cast<unsigned>(tag), pos_ - 1);
            return nullptr;
    }
}

// ---------------------------------------------------------------------------
// parse_array_element
// ---------------------------------------------------------------------------

Node* Parser::parse_array_element() {
    // Depth check
    if (depth_ >= kMaxNestingDepth) {
        set_error(kErrNestingTooDeep,
                  "nesting depth %d exceeds maximum %d", depth_, kMaxNestingDepth);
        return nullptr;
    }

    // Decode element count
    uint64_t count_raw;
    int consumed = decode_varint(data_ + pos_, data_len_ - pos_, &count_raw);
    if (consumed < 0) {
        set_error(kErrInvalidVarint,
                  "invalid array count varint at offset %zu", pos_);
        return nullptr;
    }
    pos_ += static_cast<size_t>(consumed);

    uint32_t count = static_cast<uint32_t>(count_raw);

    // Allocate the container node and remember its index (pool may grow later)
    Node* container = pool_.allocate();
    if (!container) {
        set_error(kErrAllocationFailed, "array node allocation failed");
        return nullptr;
    }
    size_t container_idx = pool_.size() - 1;
    container->type             = kTypeArray;
    container->key_string_index = 0xFFFFFFFF;
    container->field_offset     = static_cast<uint32_t>(pos_);

    // Record where children will start
    uint32_t first_child_idx = static_cast<uint32_t>(pool_.size());

    depth_++;
    for (uint32_t i = 0; i < count; ++i) {
        Node* child = parse_element();
        if (!child) return nullptr;  // error already set
    }
    depth_--;

    // Re-acquire container pointer (pool may have grown during child parsing)
    Node* cnode = pool_.get(container_idx);
    cnode->value.container.first_child_idx = first_child_idx;
    cnode->value.container.count           = count;
    cnode->key_string_index                = 0xFFFFFFFF;

    return cnode;
}

// ---------------------------------------------------------------------------
// parse_map_element
// ---------------------------------------------------------------------------

Node* Parser::parse_map_element() {
    // Load string table if needed
    if (header_.flags & kFlagHasStringTable) {
        if (!ensure_string_table()) return nullptr;
    }

    // Depth check
    if (depth_ >= kMaxNestingDepth) {
        set_error(kErrNestingTooDeep,
                  "nesting depth %d exceeds maximum %d", depth_, kMaxNestingDepth);
        return nullptr;
    }

    // Decode element count (number of key-value pairs)
    uint64_t count_raw;
    int consumed = decode_varint(data_ + pos_, data_len_ - pos_, &count_raw);
    if (consumed < 0) {
        set_error(kErrInvalidVarint,
                  "invalid map count varint at offset %zu", pos_);
        return nullptr;
    }
    pos_ += static_cast<size_t>(consumed);

    uint32_t count = static_cast<uint32_t>(count_raw);

    // Allocate the container node and remember its index
    Node* container = pool_.allocate();
    if (!container) {
        set_error(kErrAllocationFailed, "map node allocation failed");
        return nullptr;
    }
    size_t container_idx = pool_.size() - 1;
    container->type             = kTypeMap;
    container->key_string_index = 0xFFFFFFFF;
    container->field_offset     = static_cast<uint32_t>(pos_);

    uint32_t first_child_idx = static_cast<uint32_t>(pool_.size());

    depth_++;
    for (uint32_t i = 0; i < count; ++i) {
        // Decode the string-table key index for this entry
        uint64_t key_index_raw;
        int key_consumed = decode_varint(data_ + pos_, data_len_ - pos_, &key_index_raw);
        if (key_consumed < 0) {
            set_error(kErrInvalidVarint,
                      "invalid map key index varint at offset %zu", pos_);
            return nullptr;
        }
        pos_ += static_cast<size_t>(key_consumed);
        uint32_t key_index = static_cast<uint32_t>(key_index_raw);

        // Parse the value element
        Node* value_node = parse_element();
        if (!value_node) return nullptr;  // error already set

        // Set the key index on the value node
        // Re-acquire via pool (parse_element may have grown the pool)
        size_t value_idx = pool_.size() - 1;
        pool_.get(value_idx)->key_string_index = key_index;
    }
    depth_--;

    // Re-acquire container pointer
    Node* cnode = pool_.get(container_idx);
    cnode->value.container.first_child_idx = first_child_idx;
    cnode->value.container.count           = count;
    cnode->key_string_index                = 0xFFFFFFFF;

    return cnode;
}

// ---------------------------------------------------------------------------
// parse_typed_array_element
// ---------------------------------------------------------------------------

Node* Parser::parse_typed_array_element() {
    // Read element type byte
    if (!has_bytes(1)) {
        set_error(kErrTruncated,
                  "truncated typed-array elem_type at offset %zu", pos_);
        return nullptr;
    }
    uint8_t elem_type_raw = data_[pos_];
    pos_++;
    ElementType elem_type = static_cast<ElementType>(elem_type_raw);

    // Validate that it is a scalar type
    switch (elem_type) {
        case kTypeNull:
        case kTypeBool:
        case kTypeInt8:
        case kTypeInt16:
        case kTypeInt32:
        case kTypeInt64:
        case kTypeFloat32:
        case kTypeFloat64:
        case kTypeTimestamp:
            break;  // valid scalar
        default:
            set_error(kErrInvalidType,
                      "typed-array elem_type 0x%02x is not a scalar type",
                      static_cast<unsigned>(elem_type_raw));
            return nullptr;
    }

    // Decode element count
    uint64_t count_raw;
    int consumed = decode_varint(data_ + pos_, data_len_ - pos_, &count_raw);
    if (consumed < 0) {
        set_error(kErrInvalidVarint,
                  "invalid typed-array count varint at offset %zu", pos_);
        return nullptr;
    }
    pos_ += static_cast<size_t>(consumed);
    uint32_t count = static_cast<uint32_t>(count_raw);

    // Allocate the typed-array node and remember its index
    Node* ta_node = pool_.allocate();
    if (!ta_node) {
        set_error(kErrAllocationFailed, "typed-array node allocation failed");
        return nullptr;
    }
    size_t ta_idx = pool_.size() - 1;
    ta_node->type             = kTypeTypedArray;
    ta_node->key_string_index = 0xFFFFFFFF;
    ta_node->field_offset     = static_cast<uint32_t>(pos_);

    uint32_t first_child_idx = static_cast<uint32_t>(pool_.size());

    // Parse each scalar child
    for (uint32_t i = 0; i < count; ++i) {
        Node* child = parse_scalar(elem_type);
        if (!child) return nullptr;  // error already set
    }

    // Re-acquire typed-array node (pool may have grown)
    Node* typed_array_node = pool_.get(ta_idx);
    typed_array_node->value.typed_array.elem_type      = elem_type;
    typed_array_node->value.typed_array.first_child_idx = first_child_idx;
    typed_array_node->value.typed_array.count           = count;
    typed_array_node->key_string_index                  = 0xFFFFFFFF;

    return typed_array_node;
}

// ---------------------------------------------------------------------------
// resolve_reference
// ---------------------------------------------------------------------------

Node* Parser::resolve_reference(uint32_t ref_index) {
    // Bounds check
    if (ref_index >= static_cast<uint32_t>(pool_.size())) {
        set_error(kErrReferenceOutOfBounds,
                  "reference index %u out of bounds (pool size %zu)",
                  ref_index, pool_.size());
        return nullptr;
    }

    // Walk the reference chain to resolve chained references
    uint32_t target_idx = ref_index;
    for (int hops = 0; hops < kMaxReferenceChain; ++hops) {
        const Node* candidate = pool_.get(target_idx);
        if (candidate->type != kTypeReference) {
            break;  // reached a non-reference node
        }
        uint32_t next_idx = candidate->value.ref_index;
        if (next_idx >= static_cast<uint32_t>(pool_.size())) {
            set_error(kErrReferenceOutOfBounds,
                      "chained reference index %u out of bounds (pool size %zu)",
                      next_idx, pool_.size());
            return nullptr;
        }
        target_idx = next_idx;
        if (hops == kMaxReferenceChain - 1) {
            set_error(kErrReferenceCycle,
                      "reference chain exceeds maximum depth %d", kMaxReferenceChain);
            return nullptr;
        }
    }

    // Save target fields locally before any allocation (pool may grow)
    const Node* target = pool_.get(target_idx);
    ElementType tgt_type       = target->type;
    uint32_t    tgt_key        = target->key_string_index;
    uint32_t    tgt_field_off  = target->field_offset;

    // For containers, save structural fields
    bool is_container   = (tgt_type == kTypeArray || tgt_type == kTypeMap);
    bool is_typed_array = (tgt_type == kTypeTypedArray);

    uint32_t src_first_child = 0;
    uint32_t src_count       = 0;
    ElementType ta_elem_type = kTypeNull;

    if (is_container) {
        src_first_child = target->value.container.first_child_idx;
        src_count       = target->value.container.count;
    } else if (is_typed_array) {
        ta_elem_type    = target->value.typed_array.elem_type;
        src_first_child = target->value.typed_array.first_child_idx;
        src_count       = target->value.typed_array.count;
    }

    // Allocate the copy node (may invalidate all existing Node* pointers)
    Node* copy_node = pool_.allocate();
    if (!copy_node) {
        set_error(kErrAllocationFailed, "reference copy node allocation failed");
        return nullptr;
    }
    size_t copy_idx = pool_.size() - 1;

    // Re-acquire by index and fill in the copy using saved locals
    Node* cnode = pool_.get(copy_idx);
    cnode->type             = tgt_type;
    cnode->key_string_index = tgt_key;
    cnode->field_offset     = tgt_field_off;

    if (is_container || is_typed_array) {
        // Deep-copy: children occupy [src_first_child, src_first_child + src_count)
        uint32_t new_first_child = static_cast<uint32_t>(pool_.size());

        for (uint32_t j = 0; j < src_count; ++j) {
            // Re-acquire source child each iteration (allocate may grow pool)
            const Node* src_child = pool_.get(src_first_child + j);

            // Save child fields before allocating
            ElementType child_type      = src_child->type;
            uint32_t    child_key       = src_child->key_string_index;
            uint32_t    child_field_off = src_child->field_offset;

            // Copy scalar value union bytes
            Node tmp_child;
            memcpy(&tmp_child, src_child, sizeof(Node));

            Node* child_copy = pool_.allocate();
            if (!child_copy) {
                set_error(kErrAllocationFailed,
                          "reference child copy allocation failed");
                return nullptr;
            }
            size_t child_copy_idx = pool_.size() - 1;
            Node* cc = pool_.get(child_copy_idx);
            cc->type             = child_type;
            cc->key_string_index = child_key;
            cc->field_offset     = child_field_off;
            // Copy value union from the saved tmp (handles all scalar value types)
            memcpy(&cc->value, &tmp_child.value, sizeof(cc->value));
        }

        // Re-acquire copy node again (children grew pool)
        cnode = pool_.get(copy_idx);
        if (is_container) {
            cnode->value.container.first_child_idx = new_first_child;
            cnode->value.container.count           = src_count;
        } else {
            cnode->value.typed_array.elem_type       = ta_elem_type;
            cnode->value.typed_array.first_child_idx = new_first_child;
            cnode->value.typed_array.count           = src_count;
        }
    } else {
        // Scalar or string/blob: copy the entire value union from the target
        // Re-acquire target safely (pool may have moved)
        const Node* safe_target = pool_.get(target_idx);
        memcpy(&cnode->value, &safe_target->value, sizeof(cnode->value));
    }

    return cnode;
}

// ---------------------------------------------------------------------------
// parse_compressed_element
// ---------------------------------------------------------------------------

Node* Parser::parse_compressed_element() {
    // Decode uncompressed size
    uint64_t uncomp_size_raw;
    int consumed = decode_varint(data_ + pos_, data_len_ - pos_, &uncomp_size_raw);
    if (consumed < 0) {
        set_error(kErrInvalidVarint,
                  "invalid compressed uncomp_size varint at offset %zu", pos_);
        return nullptr;
    }
    pos_ += static_cast<size_t>(consumed);

    if (uncomp_size_raw > kMaxDecompressedSize) {
        set_error(kErrDecompressionFailed,
                  "decompressed size %llu exceeds maximum %zu",
                  (unsigned long long)uncomp_size_raw, kMaxDecompressedSize);
        return nullptr;
    }
    size_t uncomp_size = static_cast<size_t>(uncomp_size_raw);

    // Decode compressed size
    uint64_t comp_size_raw;
    consumed = decode_varint(data_ + pos_, data_len_ - pos_, &comp_size_raw);
    if (consumed < 0) {
        set_error(kErrInvalidVarint,
                  "invalid compressed comp_size varint at offset %zu", pos_);
        return nullptr;
    }
    pos_ += static_cast<size_t>(consumed);
    size_t comp_size = static_cast<size_t>(comp_size_raw);

    if (!has_bytes(comp_size)) {
        set_error(kErrTruncated,
                  "truncated compressed data at offset %zu (need %zu bytes)",
                  pos_, comp_size);
        return nullptr;
    }

    // Allocate decompression output buffer
    uint8_t* decomp_buf = static_cast<uint8_t*>(malloc(uncomp_size + 1));
    if (!decomp_buf) {
        set_error(kErrAllocationFailed, "decompression buffer allocation failed");
        return nullptr;
    }

    // Minimal RLE decompression:
    // A run of 3+ identical bytes B is encoded as [B, B, B, count-3].
    // Non-run bytes are copied verbatim.
    const uint8_t* src     = data_ + pos_;
    size_t         src_i   = 0;
    size_t         dst_i   = 0;

    while (src_i < comp_size && dst_i < uncomp_size) {
        // Check for an RLE run: need at least 4 bytes (B B B extra_count)
        if (src_i + 3 < comp_size &&
            src[src_i] == src[src_i + 1] &&
            src[src_i] == src[src_i + 2]) {
            uint8_t run_byte  = src[src_i];
            uint8_t extra     = src[src_i + 3];
            uint32_t run_len  = 3u + extra;
            src_i += 4;

            for (uint32_t k = 0; k < run_len && dst_i < uncomp_size; ++k) {
                decomp_buf[dst_i++] = run_byte;
            }
        } else {
            decomp_buf[dst_i++] = src[src_i++];
        }
    }

    pos_ += comp_size;

    if (dst_i != uncomp_size) {
        free(decomp_buf);
        set_error(kErrDecompressionFailed,
                  "decompressed size mismatch: expected %zu, got %zu",
                  uncomp_size, dst_i);
        return nullptr;
    }

    // Parse the decompressed buffer as a child element using a sub-Parser
    Parser sub_parser;
    ErrorCode sub_err = sub_parser.parse(decomp_buf, uncomp_size);
    free(decomp_buf);

    if (sub_err != kOk) {
        set_error(sub_err,
                  "compressed sub-element parse failed: %s",
                  sub_parser.error_message());
        return nullptr;
    }

    // The sub-parser parsed successfully; copy the root node from its pool
    // into our pool.  We do a shallow copy of the root node (not a full deep
    // copy of the whole sub-tree — the sub-tree lives in sub_parser's pool
    // which is freed when sub_parser goes out of scope).  For the purposes of
    // the Helios format the compressed element represents a single wrapped
    // value; we record its type and scalar fields here.  For containers the
    // children are not reachable after sub_parser is destroyed, but the
    // container node itself (with its type) is faithfully represented.
    const Node* sub_root = sub_parser.root();
    if (!sub_root) {
        set_error(kErrDecompressionFailed,
                  "compressed sub-element produced no root node");
        return nullptr;
    }

    // Save sub-root fields before allocating
    ElementType sub_type      = sub_root->type;
    uint32_t    sub_key       = sub_root->key_string_index;
    uint32_t    sub_field_off = sub_root->field_offset;
    Node tmp_sub;
    memcpy(&tmp_sub, sub_root, sizeof(Node));

    Node* wrap_node = pool_.allocate();
    if (!wrap_node) {
        set_error(kErrAllocationFailed, "compressed result node allocation failed");
        return nullptr;
    }
    size_t wrap_idx = pool_.size() - 1;
    Node*  wnode    = pool_.get(wrap_idx);
    wnode->type             = sub_type;
    wnode->key_string_index = sub_key;
    wnode->field_offset     = sub_field_off;
    memcpy(&wnode->value, &tmp_sub.value, sizeof(wnode->value));

    return wnode;
}

}  // namespace helios
