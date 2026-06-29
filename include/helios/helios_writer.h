#ifndef HELIOS_WRITER_H_
#define HELIOS_WRITER_H_

#include "helios_types.h"
#include "helios_node.h"
#include <cstdint>
#include <cstddef>

namespace helios {

// ---------------------------------------------------------------------------
// WriterConfig -- knobs for controlling binary output.
// ---------------------------------------------------------------------------

struct WriterConfig {
    // If true, a CRC32 checksum footer is appended and kFlagHasChecksum
    // is set in the header.  Enabled by default.
    bool enable_checksum;

    // If true, strings used as map keys are de-duplicated into a shared
    // string table and kFlagHasStringTable is set.
    bool enable_string_table;

    // If true, elements larger than compress_threshold_bytes are
    // automatically RLE-compressed and wrapped in kTypeCompressed.
    bool enable_compression;

    // Minimum raw size (bytes) before compression is attempted.
    // Only relevant when enable_compression is true.
    size_t compress_threshold_bytes;

    // Maximum buffer size the writer is allowed to allocate.  Zero
    // means no limit.
    size_t max_output_size;

    WriterConfig();
};

// ---------------------------------------------------------------------------
// StringTableBuilder -- accumulates unique strings and serialises
// the table into the FBF string table wire format.
//
// Wire format produced:
//   varint  entry_count
//   for each entry:
//     varint  string_length
//     bytes   string_data   (no null terminator on wire)
// ---------------------------------------------------------------------------

class StringTableBuilder {
public:
    StringTableBuilder();
    ~StringTableBuilder();

    // Intern a string.  Returns an index that can be used as a
    // key_string_index in map entries.  Duplicate strings receive the
    // same index.
    uint32_t intern(const char* str, size_t len);

    // Number of unique strings interned so far.
    uint32_t count() const { return count_; }

    // Serialise the entire table into caller-supplied buffer.
    // Returns bytes written, or -1 on error.
    int serialise(uint8_t* dst, size_t dst_cap) const;

    // Upper-bound estimate of the serialised table size.
    size_t estimated_size() const;

    void reset();

private:
    // Flat array of interned strings.  We keep our own copies.
    struct Entry {
        char*  data;
        size_t length;
    };

    Entry*   entries_;
    uint32_t count_;
    uint32_t capacity_;

    void grow();

    // Linear search for a duplicate.  Fine for the string counts we
    // expect in typical FBF documents (< a few hundred).
    int find(const char* str, size_t len) const;
};

// ---------------------------------------------------------------------------
// Writer -- low-level serialiser.  Call write_header(), then emit
// elements with write_*(), then write_footer() and finalize().
// ---------------------------------------------------------------------------

class Writer {
public:
    explicit Writer(const WriterConfig& cfg = WriterConfig());
    ~Writer();

    // -- header / footer ----------------------------------------------------
    ErrorCode write_header(uint32_t element_count);
    ErrorCode write_footer();

    // -- scalar elements ----------------------------------------------------
    ErrorCode write_null();
    ErrorCode write_bool(bool v);
    ErrorCode write_int8(int8_t v);
    ErrorCode write_int16(int16_t v);
    ErrorCode write_int32(int32_t v);
    ErrorCode write_int64(int64_t v);
    ErrorCode write_float32(float v);
    ErrorCode write_float64(double v);
    ErrorCode write_timestamp(int64_t epoch_us);

    // -- variable-length elements -------------------------------------------
    ErrorCode write_string(const char* data, size_t len);
    ErrorCode write_blob(const uint8_t* data, size_t len);

    // -- containers ---------------------------------------------------------
    ErrorCode write_array_start(uint32_t count);
    ErrorCode write_array_end();
    ErrorCode write_map_start(uint32_t count);
    ErrorCode write_map_end();
    ErrorCode write_map_key(uint32_t string_index);

    // -- typed array --------------------------------------------------------
    ErrorCode write_typed_array_header(ElementType elem_type, uint32_t count);

    // -- compressed element wrapper -----------------------------------------
    // Compresses `raw_data` with RLE and emits a kTypeCompressed element.
    ErrorCode write_compressed_element(const uint8_t* raw_data, size_t raw_len);

    // -- reference ----------------------------------------------------------
    ErrorCode write_reference(uint32_t ref_index);

    // -- high-level dispatch ------------------------------------------------
    // Recursively serialise a Node and its children from a NodePool.
    ErrorCode write_element(const Node* node, const NodePool& pool);

    // -- serialise from a complete tree ------------------------------------
    // Walks the entire tree rooted at `root` and produces a fully-framed
    // FBF document (header + elements + string table + footer).
    ErrorCode write_document(const Node* root, const NodePool& pool);

    // -- output access ------------------------------------------------------
    // Returns a pointer to the output buffer.  Valid until the next
    // mutating call or destruction.
    const uint8_t* data() const { return buf_; }
    size_t size() const { return pos_; }

    // Detach the internal buffer.  Caller owns the returned memory and
    // must free() it.  After this call data() returns nullptr.
    uint8_t* detach(size_t* out_len);

    // Discard all written data and reset internal state.
    void reset();

    // Access the string table builder (e.g. for pre-interning keys).
    StringTableBuilder& string_table() { return stb_; }
    const StringTableBuilder& string_table() const { return stb_; }

    const char* error_message() const { return error_msg_; }

private:
    // Buffer management
    bool ensure_capacity(size_t additional);
    void emit_byte(uint8_t b);
    void emit_bytes(const void* src, size_t count);
    int  emit_varint(uint64_t value);

    // Patch a 4-byte little-endian value at a previously recorded offset
    void patch_u32(size_t offset, uint32_t value);

    // Error helper
    void set_error(ErrorCode code, const char* fmt, ...);

    // Count elements in a node tree recursively
    uint32_t count_elements(const Node* node, const NodePool& pool) const;

    WriterConfig  cfg_;
    uint8_t*      buf_;
    size_t        pos_;
    size_t        cap_;
    int           depth_;

    StringTableBuilder stb_;
    size_t             header_offset_;   // byte offset of the header for patching

    ErrorCode error_code_;
    char      error_msg_[512];
};

// ---------------------------------------------------------------------------
// DocumentBuilder -- fluent API for programmatically constructing an
// FBF document without manually managing a NodePool.
//
// Usage:
//   DocumentBuilder b;
//   b.add_string("hello", 5)
//    .begin_map(2)
//      .map_key("x")
//      .add_int32(42)
//      .map_key("y")
//      .add_bool(true)
//    .end_map()
//    .add_null();
//
//   uint8_t* bytes;
//   size_t   len;
//   ErrorCode err = b.build(&bytes, &len);
//   // ... use bytes ...
//   free(bytes);
// ---------------------------------------------------------------------------

class DocumentBuilder {
public:
    explicit DocumentBuilder(const WriterConfig& cfg = WriterConfig());
    ~DocumentBuilder();

    // -- scalars ------------------------------------------------------------
    DocumentBuilder& add_null();
    DocumentBuilder& add_bool(bool v);
    DocumentBuilder& add_int8(int8_t v);
    DocumentBuilder& add_int16(int16_t v);
    DocumentBuilder& add_int32(int32_t v);
    DocumentBuilder& add_int64(int64_t v);
    DocumentBuilder& add_float32(float v);
    DocumentBuilder& add_float64(double v);
    DocumentBuilder& add_timestamp(int64_t epoch_us);

    // -- variable-length ----------------------------------------------------
    DocumentBuilder& add_string(const char* data, size_t len);
    DocumentBuilder& add_blob(const uint8_t* data, size_t len);

    // -- containers ---------------------------------------------------------
    DocumentBuilder& begin_array(uint32_t count);
    DocumentBuilder& end_array();
    DocumentBuilder& begin_map(uint32_t count);
    DocumentBuilder& end_map();
    DocumentBuilder& map_key(const char* key);
    DocumentBuilder& map_key(const char* key, size_t len);

    // -- reference ----------------------------------------------------------
    DocumentBuilder& add_reference(uint32_t ref_index);

    // -- finalise -----------------------------------------------------------
    // Produces the complete FBF byte stream.  Caller must free() the
    // returned pointer.
    ErrorCode build(uint8_t** out_data, size_t* out_len);

    // Reset builder state for reuse.
    void reset();

    bool has_error() const { return error_ != kOk; }
    ErrorCode error_code() const { return error_; }

private:
    // Internally we just record operations and replay them through a
    // Writer in build().
    enum OpKind {
        kOpNull, kOpBool, kOpInt8, kOpInt16, kOpInt32, kOpInt64,
        kOpFloat32, kOpFloat64, kOpTimestamp,
        kOpString, kOpBlob,
        kOpArrayStart, kOpArrayEnd,
        kOpMapStart, kOpMapEnd, kOpMapKey,
        kOpReference
    };

    struct Op {
        OpKind kind;
        union {
            bool     bval;
            int8_t   i8;
            int16_t  i16;
            int32_t  i32;
            int64_t  i64;
            float    f32;
            double   f64;
            uint32_t u32;          // count / ref_index
        } scalar;
        // For string / blob / map_key: pointer + length into data_buf_
        size_t data_offset;
        size_t data_length;
    };

    void push_op(OpKind kind);
    void push_data(const void* src, size_t len);
    void grow_ops();
    void grow_data();
    uint32_t count_root_elements() const;

    WriterConfig cfg_;
    Op*          ops_;
    size_t       ops_count_;
    size_t       ops_cap_;

    uint8_t*     data_buf_;
    size_t       data_len_;
    size_t       data_cap_;

    ErrorCode    error_;
};

}  // namespace helios

#endif  // HELIOS_WRITER_H_
