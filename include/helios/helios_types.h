// include/helios/helios_types.h
#ifndef HELIOS_TYPES_H_
#define HELIOS_TYPES_H_
#include <cstdint>
#include <cstddef>

namespace helios {

// ---- FBF wire-format magic ------------------------------------------------
static const uint8_t kMagic[4]    = {0x48, 0x42, 0x46, 0x01}; // "HBF\x01"
static const uint8_t kEndMagic[4] = {0x48, 0x45, 0x4E, 0x44}; // "HEND"
static const uint16_t kVersion    = 0x0001;

// ---- Structural limits ----------------------------------------------------
static const size_t kHeaderSize           = 16;
static const size_t kFooterSize           = 16;
static const int    kMaxNestingDepth      = 64;
static const int    kMaxReferenceChain    = 16;
static const size_t kMaxStringLength      = 1u << 20;
static const size_t kMaxElementCount      = 1u << 20;
static const size_t kMaxDecompressedSize  = 1u << 24;
static const size_t kInitialNodePoolCap   = 24;

// ---- Element type tags ----------------------------------------------------
enum ElementType : uint8_t {
    kTypeNull       = 0x01,
    kTypeBool       = 0x02,
    kTypeInt8       = 0x03,
    kTypeInt16      = 0x04,
    kTypeInt32      = 0x05,
    kTypeInt64      = 0x06,
    kTypeFloat32    = 0x07,
    kTypeFloat64    = 0x08,
    kTypeString     = 0x09,
    kTypeBlob       = 0x0A,
    kTypeArray      = 0x0B,
    kTypeMap        = 0x0C,
    kTypeCompressed = 0x0D,
    kTypeReference  = 0x0E,
    kTypeTimestamp  = 0x0F,
    kTypeTypedArray = 0x10,
};

// ---- Header flags ---------------------------------------------------------
enum HeaderFlags : uint16_t {
    kFlagHasStringTable = 0x0001,
    kFlagHasChecksum    = 0x0002,
    kFlagHasCompression = 0x0004,
};

// ---- Error codes ----------------------------------------------------------
enum ErrorCode {
    kOk = 0,
    kErrInvalidMagic,
    kErrUnsupportedVersion,
    kErrTruncated,
    kErrInvalidVarint,
    kErrInvalidType,
    kErrNestingTooDeep,
    kErrReferenceCycle,
    kErrReferenceOutOfBounds,
    kErrChecksumMismatch,
    kErrDecompressionFailed,
    kErrStringTableCorrupt,
    kErrInvalidElementCount,
    kErrAllocationFailed,
    kErrMalformedInput,
};

// ---- Wire-format structs (packed) -----------------------------------------
#pragma pack(push, 1)
struct FileHeader {
    uint8_t  magic[4];
    uint16_t version;
    uint16_t flags;
    uint32_t element_count;
    uint32_t string_table_offset;
};  // 16 bytes total

struct FileFooter {
    uint32_t checksum;
    uint32_t total_size;
    uint32_t reserved;
    uint8_t  end_magic[4];
};  // 16 bytes total
#pragma pack(pop)

}  // namespace helios
#endif  // HELIOS_TYPES_H_
