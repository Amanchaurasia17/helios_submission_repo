#include "helios_writer.h"
#include "helios_varint.h"
#include "helios_checksum.h"
#include "helios_compress.h"
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>

namespace helios {

// ===========================================================================
// WriterConfig
// ===========================================================================

WriterConfig::WriterConfig()
    : enable_checksum(true),
      enable_string_table(true),
      enable_compression(false),
      compress_threshold_bytes(256),
      max_output_size(0) {}

// ===========================================================================
// StringTableBuilder
// ===========================================================================

static const uint32_t kInitialSTBCapacity = 32;

StringTableBuilder::StringTableBuilder()
    : entries_(nullptr), count_(0), capacity_(0) {}

StringTableBuilder::~StringTableBuilder() {
    reset();
}

void StringTableBuilder::reset() {
    for (uint32_t i = 0; i < count_; i++) {
        free(entries_[i].data);
    }
    free(entries_);
    entries_ = nullptr;
    count_ = 0;
    capacity_ = 0;
}

void StringTableBuilder::grow() {
    uint32_t new_cap = (capacity_ == 0) ? kInitialSTBCapacity : capacity_ * 2;
    Entry* fresh = static_cast<Entry*>(
        realloc(entries_, new_cap * sizeof(Entry)));
    if (!fresh) return;          // silent failure; intern() will notice
    entries_ = fresh;
    capacity_ = new_cap;
}

int StringTableBuilder::find(const char* str, size_t len) const {
    for (uint32_t i = 0; i < count_; i++) {
        if (entries_[i].length == len &&
            memcmp(entries_[i].data, str, len) == 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

uint32_t StringTableBuilder::intern(const char* str, size_t len) {
    // Check for an existing entry first.
    int existing = find(str, len);
    if (existing >= 0) return static_cast<uint32_t>(existing);

    // Need a new slot.
    if (count_ >= capacity_) {
        grow();
        if (count_ >= capacity_) return 0;   // allocation failure fallback
    }

    char* copy = static_cast<char*>(malloc(len + 1));
    if (!copy) return 0;
    memcpy(copy, str, len);
    copy[len] = '\0';

    entries_[count_].data   = copy;
    entries_[count_].length = len;

    return count_++;
}

size_t StringTableBuilder::estimated_size() const {
    // varint(count) + sum( varint(len) + len )
    size_t sz = 10;   // generous upper-bound for the count varint
    for (uint32_t i = 0; i < count_; i++) {
        sz += 10 + entries_[i].length;   // 10 bytes per varint max
    }
    return sz;
}

int StringTableBuilder::serialise(uint8_t* dst, size_t dst_cap) const {
    size_t pos = 0;

    // Entry count
    uint8_t vbuf[10];
    int vb = encode_varint(static_cast<uint64_t>(count_), vbuf, sizeof(vbuf));
    if (vb < 0) return -1;
    if (pos + static_cast<size_t>(vb) > dst_cap) return -1;
    memcpy(dst + pos, vbuf, static_cast<size_t>(vb));
    pos += static_cast<size_t>(vb);

    // Each string: varint(len) followed by raw bytes
    for (uint32_t i = 0; i < count_; i++) {
        vb = encode_varint(static_cast<uint64_t>(entries_[i].length),
                           vbuf, sizeof(vbuf));
        if (vb < 0) return -1;
        if (pos + static_cast<size_t>(vb) + entries_[i].length > dst_cap) {
            return -1;
        }
        memcpy(dst + pos, vbuf, static_cast<size_t>(vb));
        pos += static_cast<size_t>(vb);
        memcpy(dst + pos, entries_[i].data, entries_[i].length);
        pos += entries_[i].length;
    }

    return static_cast<int>(pos);
}

// ===========================================================================
// Writer -- internal helpers
// ===========================================================================

static const size_t kWriterInitialCap = 4096;

Writer::Writer(const WriterConfig& cfg)
    : cfg_(cfg), buf_(nullptr), pos_(0), cap_(0), depth_(0),
      header_offset_(0), error_code_(kOk) {
    error_msg_[0] = '\0';
}

Writer::~Writer() {
    free(buf_);
}

void Writer::reset() {
    free(buf_);
    buf_ = nullptr;
    pos_ = 0;
    cap_ = 0;
    depth_ = 0;
    header_offset_ = 0;
    error_code_ = kOk;
    error_msg_[0] = '\0';
    stb_.reset();
}

void Writer::set_error(ErrorCode code, const char* fmt, ...) {
    error_code_ = code;
    va_list args;
    va_start(args, fmt);
    vsnprintf(error_msg_, sizeof(error_msg_), fmt, args);
    va_end(args);
}

bool Writer::ensure_capacity(size_t additional) {
    size_t needed = pos_ + additional;
    if (needed <= cap_) return true;

    // Enforce the size limit if configured.
    if (cfg_.max_output_size > 0 && needed > cfg_.max_output_size) {
        set_error(kErrAllocationFailed,
                  "output would exceed max_output_size (%zu)", cfg_.max_output_size);
        return false;
    }

    size_t new_cap = (cap_ == 0) ? kWriterInitialCap : cap_;
    while (new_cap < needed) {
        new_cap *= 2;
        // Guard against overflow in the doubling loop.
        if (new_cap < cap_) {
            set_error(kErrAllocationFailed, "capacity overflow");
            return false;
        }
    }

    uint8_t* fresh = static_cast<uint8_t*>(realloc(buf_, new_cap));
    if (!fresh) {
        set_error(kErrAllocationFailed,
                  "buffer realloc failed (requested %zu)", new_cap);
        return false;
    }
    buf_ = fresh;
    cap_ = new_cap;
    return true;
}

void Writer::emit_byte(uint8_t b) {
    if (!ensure_capacity(1)) return;
    buf_[pos_++] = b;
}

void Writer::emit_bytes(const void* src, size_t count) {
    if (count == 0) return;
    if (!ensure_capacity(count)) return;
    memcpy(buf_ + pos_, src, count);
    pos_ += count;
}

int Writer::emit_varint(uint64_t value) {
    uint8_t tmp[10];
    int n = encode_varint(value, tmp, sizeof(tmp));
    if (n < 0) {
        set_error(kErrInvalidVarint, "varint encoding failed");
        return -1;
    }
    emit_bytes(tmp, static_cast<size_t>(n));
    return n;
}

void Writer::patch_u32(size_t offset, uint32_t value) {
    if (offset + 4 > pos_) return;   // shouldn't happen
    memcpy(buf_ + offset, &value, 4);
}

// ===========================================================================
// Writer -- header / footer
// ===========================================================================

ErrorCode Writer::write_header(uint32_t element_count) {
    if (element_count > kMaxElementCount) {
        set_error(kErrInvalidElementCount,
                  "element_count %u exceeds limit", element_count);
        return error_code_;
    }

    header_offset_ = pos_;

    FileHeader hdr;
    memcpy(hdr.magic, kMagic, 4);
    hdr.version = kVersion;

    // Build flag mask based on config.
    uint16_t flags = 0;
    if (cfg_.enable_string_table) flags |= kFlagHasStringTable;
    if (cfg_.enable_checksum)     flags |= kFlagHasChecksum;
    if (cfg_.enable_compression)  flags |= kFlagHasCompression;
    hdr.flags = flags;

    hdr.element_count = element_count;
    hdr.string_table_offset = 0;           // patched later if needed

    emit_bytes(&hdr, sizeof(hdr));
    return kOk;
}

ErrorCode Writer::write_footer() {
    // If a string table has entries, serialise it now and patch the header.
    if (cfg_.enable_string_table && stb_.count() > 0) {
        uint32_t st_offset = static_cast<uint32_t>(pos_);

        // Patch string_table_offset field in the header.
        // The field lives at offset header_offset_ + 12
        // (magic[4] + version[2] + flags[2] + element_count[4] = 12).
        patch_u32(header_offset_ + 12, st_offset);

        // Serialise the string table directly into our output buffer.
        size_t est = stb_.estimated_size();
        if (!ensure_capacity(est)) return error_code_;

        int written = stb_.serialise(buf_ + pos_, cap_ - pos_);
        if (written < 0) {
            set_error(kErrAllocationFailed, "string table serialisation failed");
            return error_code_;
        }
        pos_ += static_cast<size_t>(written);
    }

    if (!cfg_.enable_checksum) return kOk;

    // Compute CRC32 over everything written so far (header + elements + strtab).
    uint32_t checksum = crc32_compute(buf_, pos_);

    FileFooter ftr;
    ftr.checksum = checksum;
    ftr.total_size = static_cast<uint32_t>(pos_ + kFooterSize);
    ftr.reserved = 0;
    memcpy(ftr.end_magic, kEndMagic, 4);

    emit_bytes(&ftr, sizeof(ftr));
    return kOk;
}

// ===========================================================================
// Writer -- scalar elements
// ===========================================================================

ErrorCode Writer::write_null() {
    emit_byte(static_cast<uint8_t>(kTypeNull));
    return error_code_;
}

ErrorCode Writer::write_bool(bool v) {
    emit_byte(static_cast<uint8_t>(kTypeBool));
    emit_byte(v ? 0x01 : 0x00);
    return error_code_;
}

ErrorCode Writer::write_int8(int8_t v) {
    emit_byte(static_cast<uint8_t>(kTypeInt8));
    emit_byte(static_cast<uint8_t>(v));
    return error_code_;
}

ErrorCode Writer::write_int16(int16_t v) {
    emit_byte(static_cast<uint8_t>(kTypeInt16));
    emit_bytes(&v, 2);
    return error_code_;
}

ErrorCode Writer::write_int32(int32_t v) {
    emit_byte(static_cast<uint8_t>(kTypeInt32));
    emit_bytes(&v, 4);
    return error_code_;
}

ErrorCode Writer::write_int64(int64_t v) {
    emit_byte(static_cast<uint8_t>(kTypeInt64));
    emit_bytes(&v, 8);
    return error_code_;
}

ErrorCode Writer::write_float32(float v) {
    emit_byte(static_cast<uint8_t>(kTypeFloat32));
    emit_bytes(&v, 4);
    return error_code_;
}

ErrorCode Writer::write_float64(double v) {
    emit_byte(static_cast<uint8_t>(kTypeFloat64));
    emit_bytes(&v, 8);
    return error_code_;
}

ErrorCode Writer::write_timestamp(int64_t epoch_us) {
    emit_byte(static_cast<uint8_t>(kTypeTimestamp));
    emit_bytes(&epoch_us, 8);
    return error_code_;
}

// ===========================================================================
// Writer -- variable-length elements
// ===========================================================================

ErrorCode Writer::write_string(const char* data, size_t len) {
    if (len > kMaxStringLength) {
        set_error(kErrMalformedInput,
                  "string length %zu exceeds maximum", len);
        return error_code_;
    }
    emit_byte(static_cast<uint8_t>(kTypeString));
    emit_varint(static_cast<uint64_t>(len));
    if (error_code_ != kOk) return error_code_;
    emit_bytes(data, len);
    return error_code_;
}

ErrorCode Writer::write_blob(const uint8_t* data, size_t len) {
    if (len > kMaxDecompressedSize) {
        set_error(kErrMalformedInput,
                  "blob length %zu exceeds maximum", len);
        return error_code_;
    }
    emit_byte(static_cast<uint8_t>(kTypeBlob));
    emit_varint(static_cast<uint64_t>(len));
    if (error_code_ != kOk) return error_code_;
    if (len > 0) emit_bytes(data, len);
    return error_code_;
}

// ===========================================================================
// Writer -- containers
// ===========================================================================

ErrorCode Writer::write_array_start(uint32_t count) {
    if (depth_ >= kMaxNestingDepth) {
        set_error(kErrNestingTooDeep,
                  "array nesting depth %d exceeds limit", depth_);
        return error_code_;
    }
    emit_byte(static_cast<uint8_t>(kTypeArray));
    emit_varint(static_cast<uint64_t>(count));
    depth_++;
    return error_code_;
}

ErrorCode Writer::write_array_end() {
    if (depth_ > 0) depth_--;
    return error_code_;
}

ErrorCode Writer::write_map_start(uint32_t count) {
    if (depth_ >= kMaxNestingDepth) {
        set_error(kErrNestingTooDeep,
                  "map nesting depth %d exceeds limit", depth_);
        return error_code_;
    }
    emit_byte(static_cast<uint8_t>(kTypeMap));
    emit_varint(static_cast<uint64_t>(count));
    depth_++;
    return error_code_;
}

ErrorCode Writer::write_map_end() {
    if (depth_ > 0) depth_--;
    return error_code_;
}

ErrorCode Writer::write_map_key(uint32_t string_index) {
    // Map key is emitted as a varint string-table index preceding
    // the value element.
    emit_varint(static_cast<uint64_t>(string_index));
    return error_code_;
}

// ===========================================================================
// Writer -- typed array
// ===========================================================================

ErrorCode Writer::write_typed_array_header(ElementType elem_type,
                                           uint32_t count) {
    if (depth_ >= kMaxNestingDepth) {
        set_error(kErrNestingTooDeep,
                  "typed array nesting depth %d exceeds limit", depth_);
        return error_code_;
    }
    emit_byte(static_cast<uint8_t>(kTypeTypedArray));
    emit_byte(static_cast<uint8_t>(elem_type));
    emit_varint(static_cast<uint64_t>(count));
    depth_++;
    return error_code_;
}

// ===========================================================================
// Writer -- compressed element
// ===========================================================================

ErrorCode Writer::write_compressed_element(const uint8_t* raw_data,
                                           size_t raw_len) {
    if (raw_len == 0) {
        set_error(kErrMalformedInput, "cannot compress zero-length data");
        return error_code_;
    }

    // Allocate a temporary buffer for RLE output. In the worst case
    // RLE-encoding can expand the data slightly, so we give it 2x room.
    size_t comp_cap = raw_len * 2 + 64;
    uint8_t* comp_buf = static_cast<uint8_t*>(malloc(comp_cap));
    if (!comp_buf) {
        set_error(kErrAllocationFailed,
                  "compression buffer allocation failed (%zu)", comp_cap);
        return error_code_;
    }

    int comp_len = rle_compress(raw_data, raw_len, comp_buf, comp_cap);
    if (comp_len < 0) {
        free(comp_buf);
        set_error(kErrDecompressionFailed, "RLE compression failed");
        return error_code_;
    }

    // Emit: type tag | varint(uncompressed_size) | varint(compressed_size) | data
    emit_byte(static_cast<uint8_t>(kTypeCompressed));
    emit_varint(static_cast<uint64_t>(raw_len));
    emit_varint(static_cast<uint64_t>(comp_len));
    if (error_code_ == kOk) {
        emit_bytes(comp_buf, static_cast<size_t>(comp_len));
    }

    free(comp_buf);
    return error_code_;
}

// ===========================================================================
// Writer -- reference
// ===========================================================================

ErrorCode Writer::write_reference(uint32_t ref_index) {
    emit_byte(static_cast<uint8_t>(kTypeReference));
    emit_varint(static_cast<uint64_t>(ref_index));
    return error_code_;
}

// ===========================================================================
// Writer -- high-level element dispatch
// ===========================================================================

ErrorCode Writer::write_element(const Node* node, const NodePool& pool) {
    if (!node) {
        set_error(kErrMalformedInput, "null node pointer");
        return error_code_;
    }

    switch (node->type) {
        case kTypeNull:
            return write_null();

        case kTypeBool:
            return write_bool(node->value.bool_val);

        case kTypeInt8:
            return write_int8(node->value.i8_val);

        case kTypeInt16:
            return write_int16(node->value.i16_val);

        case kTypeInt32:
            return write_int32(node->value.i32_val);

        case kTypeInt64:
            return write_int64(node->value.i64_val);

        case kTypeFloat32:
            return write_float32(node->value.f32_val);

        case kTypeFloat64:
            return write_float64(node->value.f64_val);

        case kTypeTimestamp:
            return write_timestamp(node->value.timestamp_val);

        case kTypeString:
            return write_string(node->value.str.data, node->value.str.len);

        case kTypeBlob:
            return write_blob(
                reinterpret_cast<const uint8_t*>(node->value.str.data),
                node->value.str.len);

        case kTypeArray: {
            uint32_t count = node->value.container.count;
            uint32_t base  = node->value.container.first_child_idx;
            ErrorCode ec = write_array_start(count);
            if (ec != kOk) return ec;
            for (uint32_t i = 0; i < count; i++) {
                const Node* child = pool.get(base + i);
                if (!child) {
                    set_error(kErrMalformedInput,
                              "array child %u (pool idx %u) is null",
                              i, base + i);
                    return error_code_;
                }
                ec = write_element(child, pool);
                if (ec != kOk) return ec;
            }
            return write_array_end();
        }

        case kTypeMap: {
            uint32_t count = node->value.container.count;
            uint32_t base  = node->value.container.first_child_idx;
            ErrorCode ec = write_map_start(count);
            if (ec != kOk) return ec;
            for (uint32_t i = 0; i < count; i++) {
                const Node* child = pool.get(base + i);
                if (!child) {
                    set_error(kErrMalformedInput,
                              "map child %u (pool idx %u) is null",
                              i, base + i);
                    return error_code_;
                }
                // Emit the key string index before each value.
                ec = write_map_key(child->key_string_index);
                if (ec != kOk) return ec;
                ec = write_element(child, pool);
                if (ec != kOk) return ec;
            }
            return write_map_end();
        }

        case kTypeCompressed: {
            // The parser has already decompressed the inner node into a
            // regular element, so we re-serialise it then compress.
            // Temporarily serialise the child into a scratch buffer.
            WriterConfig inner_cfg;
            Writer inner_writer(inner_cfg);
            // Skip header/footer for the inner serialisation.
            uint32_t count = node->value.container.count;
            uint32_t base  = node->value.container.first_child_idx;
            for (uint32_t i = 0; i < count; i++) {
                const Node* child = pool.get(base + i);
                if (!child) break;
                inner_writer.write_element(child, pool);
            }
            if (inner_writer.size() > 0) {
                return write_compressed_element(inner_writer.data(),
                                                inner_writer.size());
            }
            // Fallback: emit as a null if the inner tree was empty.
            return write_null();
        }

        case kTypeReference:
            return write_reference(node->value.ref_index);

        case kTypeTypedArray: {
            ElementType etype = node->value.typed_array.elem_type;
            uint32_t count    = node->value.typed_array.count;
            uint32_t base     = node->value.typed_array.first_child_idx;
            ErrorCode ec = write_typed_array_header(etype, count);
            if (ec != kOk) return ec;
            for (uint32_t i = 0; i < count; i++) {
                const Node* child = pool.get(base + i);
                if (!child) {
                    set_error(kErrMalformedInput,
                              "typed array child %u is null", i);
                    return error_code_;
                }
                // Typed-array children are scalars; emit value only,
                // without a redundant type tag, matching how the parser
                // reads them via parse_scalar (which consumes a type tag).
                // But since write_element emits the tag, and the parser
                // actually stores individual scalar nodes, we just call
                // write_element here for correctness with the parse loop.
                ec = write_element(child, pool);
                if (ec != kOk) return ec;
            }
            // Typed arrays are closed implicitly by count.
            if (depth_ > 0) depth_--;
            return kOk;
        }

        default:
            set_error(kErrInvalidType,
                      "unknown element type 0x%02x", node->type);
            return error_code_;
    }
}

// ===========================================================================
// Writer -- element counting
// ===========================================================================

uint32_t Writer::count_elements(const Node* node,
                                const NodePool& pool) const {
    if (!node) return 0;

    uint32_t total = 1;   // count the node itself

    switch (node->type) {
        case kTypeArray:
        case kTypeMap: {
            uint32_t count = node->value.container.count;
            uint32_t base  = node->value.container.first_child_idx;
            for (uint32_t i = 0; i < count; i++) {
                const Node* child = pool.get(base + i);
                if (child) total += count_elements(child, pool);
            }
            break;
        }
        case kTypeTypedArray: {
            uint32_t count = node->value.typed_array.count;
            uint32_t base  = node->value.typed_array.first_child_idx;
            for (uint32_t i = 0; i < count; i++) {
                const Node* child = pool.get(base + i);
                if (child) total += count_elements(child, pool);
            }
            break;
        }
        default:
            break;
    }

    return total;
}

// ===========================================================================
// Writer -- full document serialisation
// ===========================================================================

ErrorCode Writer::write_document(const Node* root, const NodePool& pool) {
    if (!root) {
        set_error(kErrMalformedInput, "root node is null");
        return error_code_;
    }

    // Pre-scan the tree to count elements for the header.
    uint32_t elem_count = count_elements(root, pool);

    // If string table is enabled, pre-scan map nodes and intern their
    // key strings into the builder.  The parser stores key_string_index
    // values that reference the original string table; we preserve them
    // as-is.  A more thorough implementation would rebuild from scratch,
    // but for round-tripping this is correct.

    ErrorCode ec = write_header(elem_count);
    if (ec != kOk) return ec;

    ec = write_element(root, pool);
    if (ec != kOk) return ec;

    ec = write_footer();
    return ec;
}

// ===========================================================================
// Writer -- output management
// ===========================================================================

uint8_t* Writer::detach(size_t* out_len) {
    if (out_len) *out_len = pos_;
    uint8_t* tmp = buf_;
    buf_ = nullptr;
    pos_ = 0;
    cap_ = 0;
    return tmp;
}

// ===========================================================================
// DocumentBuilder
// ===========================================================================

static const size_t kBuilderInitOps  = 64;
static const size_t kBuilderInitData = 512;

DocumentBuilder::DocumentBuilder(const WriterConfig& cfg)
    : cfg_(cfg),
      ops_(nullptr), ops_count_(0), ops_cap_(0),
      data_buf_(nullptr), data_len_(0), data_cap_(0),
      error_(kOk) {}

DocumentBuilder::~DocumentBuilder() {
    free(ops_);
    free(data_buf_);
}

void DocumentBuilder::reset() {
    free(ops_);
    ops_ = nullptr;
    ops_count_ = 0;
    ops_cap_ = 0;

    free(data_buf_);
    data_buf_ = nullptr;
    data_len_ = 0;
    data_cap_ = 0;

    error_ = kOk;
}

// ---------------------------------------------------------------------------
// Op / data buffer management
// ---------------------------------------------------------------------------

void DocumentBuilder::grow_ops() {
    size_t new_cap = (ops_cap_ == 0) ? kBuilderInitOps : ops_cap_ * 2;
    Op* fresh = static_cast<Op*>(realloc(ops_, new_cap * sizeof(Op)));
    if (!fresh) {
        error_ = kErrAllocationFailed;
        return;
    }
    ops_ = fresh;
    ops_cap_ = new_cap;
}

void DocumentBuilder::grow_data() {
    size_t new_cap = (data_cap_ == 0) ? kBuilderInitData : data_cap_ * 2;
    uint8_t* fresh = static_cast<uint8_t*>(realloc(data_buf_, new_cap));
    if (!fresh) {
        error_ = kErrAllocationFailed;
        return;
    }
    data_buf_ = fresh;
    data_cap_ = new_cap;
}

void DocumentBuilder::push_op(OpKind kind) {
    if (error_ != kOk) return;
    if (ops_count_ >= ops_cap_) {
        grow_ops();
        if (error_ != kOk) return;
    }
    Op& op = ops_[ops_count_++];
    memset(&op, 0, sizeof(Op));
    op.kind = kind;
}

void DocumentBuilder::push_data(const void* src, size_t len) {
    if (error_ != kOk || len == 0) return;

    while (data_len_ + len > data_cap_) {
        grow_data();
        if (error_ != kOk) return;
    }

    memcpy(data_buf_ + data_len_, src, len);

    // Patch the most-recently pushed op with the data location.
    if (ops_count_ > 0) {
        ops_[ops_count_ - 1].data_offset = data_len_;
        ops_[ops_count_ - 1].data_length = len;
    }

    data_len_ += len;
}

// ---------------------------------------------------------------------------
// Scalar helpers
// ---------------------------------------------------------------------------

DocumentBuilder& DocumentBuilder::add_null() {
    push_op(kOpNull);
    return *this;
}

DocumentBuilder& DocumentBuilder::add_bool(bool v) {
    push_op(kOpBool);
    if (ops_count_ > 0) ops_[ops_count_ - 1].scalar.bval = v;
    return *this;
}

DocumentBuilder& DocumentBuilder::add_int8(int8_t v) {
    push_op(kOpInt8);
    if (ops_count_ > 0) ops_[ops_count_ - 1].scalar.i8 = v;
    return *this;
}

DocumentBuilder& DocumentBuilder::add_int16(int16_t v) {
    push_op(kOpInt16);
    if (ops_count_ > 0) ops_[ops_count_ - 1].scalar.i16 = v;
    return *this;
}

DocumentBuilder& DocumentBuilder::add_int32(int32_t v) {
    push_op(kOpInt32);
    if (ops_count_ > 0) ops_[ops_count_ - 1].scalar.i32 = v;
    return *this;
}

DocumentBuilder& DocumentBuilder::add_int64(int64_t v) {
    push_op(kOpInt64);
    if (ops_count_ > 0) ops_[ops_count_ - 1].scalar.i64 = v;
    return *this;
}

DocumentBuilder& DocumentBuilder::add_float32(float v) {
    push_op(kOpFloat32);
    if (ops_count_ > 0) ops_[ops_count_ - 1].scalar.f32 = v;
    return *this;
}

DocumentBuilder& DocumentBuilder::add_float64(double v) {
    push_op(kOpFloat64);
    if (ops_count_ > 0) ops_[ops_count_ - 1].scalar.f64 = v;
    return *this;
}

DocumentBuilder& DocumentBuilder::add_timestamp(int64_t epoch_us) {
    push_op(kOpTimestamp);
    if (ops_count_ > 0) ops_[ops_count_ - 1].scalar.i64 = epoch_us;
    return *this;
}

// ---------------------------------------------------------------------------
// Variable-length helpers
// ---------------------------------------------------------------------------

DocumentBuilder& DocumentBuilder::add_string(const char* data, size_t len) {
    push_op(kOpString);
    push_data(data, len);
    return *this;
}

DocumentBuilder& DocumentBuilder::add_blob(const uint8_t* data, size_t len) {
    push_op(kOpBlob);
    push_data(data, len);
    return *this;
}

// ---------------------------------------------------------------------------
// Container helpers
// ---------------------------------------------------------------------------

DocumentBuilder& DocumentBuilder::begin_array(uint32_t count) {
    push_op(kOpArrayStart);
    if (ops_count_ > 0) ops_[ops_count_ - 1].scalar.u32 = count;
    return *this;
}

DocumentBuilder& DocumentBuilder::end_array() {
    push_op(kOpArrayEnd);
    return *this;
}

DocumentBuilder& DocumentBuilder::begin_map(uint32_t count) {
    push_op(kOpMapStart);
    if (ops_count_ > 0) ops_[ops_count_ - 1].scalar.u32 = count;
    return *this;
}

DocumentBuilder& DocumentBuilder::end_map() {
    push_op(kOpMapEnd);
    return *this;
}

DocumentBuilder& DocumentBuilder::map_key(const char* key) {
    return map_key(key, strlen(key));
}

DocumentBuilder& DocumentBuilder::map_key(const char* key, size_t len) {
    push_op(kOpMapKey);
    push_data(key, len);
    return *this;
}

// ---------------------------------------------------------------------------
// Reference
// ---------------------------------------------------------------------------

DocumentBuilder& DocumentBuilder::add_reference(uint32_t ref_index) {
    push_op(kOpReference);
    if (ops_count_ > 0) ops_[ops_count_ - 1].scalar.u32 = ref_index;
    return *this;
}

// ---------------------------------------------------------------------------
// count_root_elements -- walks the recorded ops to figure out how many
// top-level elements were added (needed for the header).
// ---------------------------------------------------------------------------

uint32_t DocumentBuilder::count_root_elements() const {
    uint32_t total = 0;
    int depth = 0;

    for (size_t i = 0; i < ops_count_; i++) {
        switch (ops_[i].kind) {
            case kOpArrayStart:
            case kOpMapStart:
                // Count the container as one element at its opening depth,
                // then increase depth so children are not double-counted.
                total++;
                depth++;
                break;

            case kOpArrayEnd:
            case kOpMapEnd:
                if (depth > 0) depth--;
                break;

            case kOpMapKey:
                // Map keys don't count as separate elements.
                break;

            default:
                // Scalar or leaf element.
                total++;
                break;
        }
    }

    return total;
}

// ---------------------------------------------------------------------------
// build -- replay the operation stream through a Writer
// ---------------------------------------------------------------------------

ErrorCode DocumentBuilder::build(uint8_t** out_data, size_t* out_len) {
    if (error_ != kOk) return error_;
    if (!out_data || !out_len) return kErrMalformedInput;

    *out_data = nullptr;
    *out_len  = 0;

    Writer w(cfg_);

    // First pass: intern all map keys into the writer's string table
    // so indices are available during the write pass.
    if (cfg_.enable_string_table) {
        for (size_t i = 0; i < ops_count_; i++) {
            if (ops_[i].kind == kOpMapKey && ops_[i].data_length > 0) {
                const char* kdata = reinterpret_cast<const char*>(
                    data_buf_ + ops_[i].data_offset);
                uint32_t idx = w.string_table().intern(kdata,
                                                       ops_[i].data_length);
                // Stash the resolved index back into the op for the
                // write pass so we don't have to look it up again.
                ops_[i].scalar.u32 = idx;
            }
        }
    }

    // Compute element count for the header.
    uint32_t elem_count = count_root_elements();

    ErrorCode ec = w.write_header(elem_count);
    if (ec != kOk) return ec;

    // Replay ops.
    for (size_t i = 0; i < ops_count_; i++) {
        const Op& op = ops_[i];

        switch (op.kind) {
            case kOpNull:
                ec = w.write_null();
                break;
            case kOpBool:
                ec = w.write_bool(op.scalar.bval);
                break;
            case kOpInt8:
                ec = w.write_int8(op.scalar.i8);
                break;
            case kOpInt16:
                ec = w.write_int16(op.scalar.i16);
                break;
            case kOpInt32:
                ec = w.write_int32(op.scalar.i32);
                break;
            case kOpInt64:
                ec = w.write_int64(op.scalar.i64);
                break;
            case kOpFloat32:
                ec = w.write_float32(op.scalar.f32);
                break;
            case kOpFloat64:
                ec = w.write_float64(op.scalar.f64);
                break;
            case kOpTimestamp:
                ec = w.write_timestamp(op.scalar.i64);
                break;

            case kOpString: {
                const char* sdata = reinterpret_cast<const char*>(
                    data_buf_ + op.data_offset);
                ec = w.write_string(sdata, op.data_length);
                break;
            }
            case kOpBlob: {
                const uint8_t* bdata = data_buf_ + op.data_offset;
                ec = w.write_blob(bdata, op.data_length);
                break;
            }

            case kOpArrayStart:
                ec = w.write_array_start(op.scalar.u32);
                break;
            case kOpArrayEnd:
                ec = w.write_array_end();
                break;

            case kOpMapStart:
                ec = w.write_map_start(op.scalar.u32);
                break;
            case kOpMapEnd:
                ec = w.write_map_end();
                break;
            case kOpMapKey:
                ec = w.write_map_key(op.scalar.u32);
                break;

            case kOpReference:
                ec = w.write_reference(op.scalar.u32);
                break;
        }

        if (ec != kOk) return ec;
    }

    // Finalise: write the footer (string table + checksum).
    ec = w.write_footer();
    if (ec != kOk) return ec;

    *out_data = w.detach(out_len);
    return kOk;
}

}  // namespace helios
