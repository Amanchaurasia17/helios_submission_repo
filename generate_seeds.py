import os
import struct
import zlib

def encode_varint(value):
    buf = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        if value != 0:
            byte |= 0x80
        buf.append(byte)
        if value == 0:
            break
    return bytes(buf)

def crc32_of(data):
    return zlib.crc32(data) & 0xffffffff

def build_header(flags, element_count, string_table_offset=0):
    kMagic = b'HBF\x01'
    kVersion = 0x0001
    return struct.pack('<4sHHII', kMagic, kVersion, flags, element_count, string_table_offset)

def build_footer(data_body):
    checksum = crc32_of(data_body)
    total_size = 16 + len(data_body) + 16
    reserved = 0
    kEndMagic = b'HEND'
    return struct.pack('<IIIS4s', checksum, total_size, reserved, kEndMagic)[:16] # Wait! struct pack for end magic
    # Actually: struct.pack('<IIIS4s') is wrong format.
    # The footer is:
    # struct FileFooter {
    #     uint32_t checksum;
    #     uint32_t total_size;
    #     uint32_t reserved;
    #     uint8_t  end_magic[4];
    # };
    # Let's pack it as:
    # struct.pack('<III4s', checksum, total_size, reserved, kEndMagic)
    # This is exactly 16 bytes. Let's write that.

# Corrected build_footer:
def build_footer(data_body):
    checksum = crc32_of(data_body)
    total_size = 16 + len(data_body) + 16
    reserved = 0
    kEndMagic = b'HEND'
    return struct.pack('<III4s', checksum, total_size, reserved, kEndMagic)

def rle_compress(data):
    res = bytearray()
    i = 0
    n = len(data)
    while i < n:
        run_len = 1
        while i + run_len < n and data[i + run_len] == data[i] and run_len < 258:
            run_len += 1
        if run_len >= 3:
            res.append(data[i])
            res.append(data[i])
            res.append(data[i])
            res.append(run_len - 3)
            i += run_len
        else:
            res.append(data[i])
            i += 1
    return bytes(res)

# Define element type tags
kTypeNull = 0x01
kTypeBool = 0x02
kTypeInt8 = 0x03
kTypeInt16 = 0x04
kTypeInt32 = 0x05
kTypeInt64 = 0x06
kTypeFloat32 = 0x07
kTypeFloat64 = 0x08
kTypeString = 0x09
kTypeBlob = 0x0A
kTypeArray = 0x0B
kTypeMap = 0x0C
kTypeCompressed = 0x0D
kTypeReference = 0x0E
kTypeTimestamp = 0x0F
kTypeTypedArray = 0x10

# Header Flags
kFlagHasStringTable = 0x0001
kFlagHasChecksum = 0x0002
kFlagHasCompression = 0x0004

def generate_minimal_null():
    body = bytes([kTypeNull])
    header = build_header(kFlagHasChecksum, 1, 0)
    footer = build_footer(body)
    return header + body + footer

def generate_array_int32():
    body = bytearray()
    body.append(kTypeArray)
    body.extend(encode_varint(3))
    # 3 int32 elements
    for val in [10, 20, 30]:
        body.append(kTypeInt32)
        body.extend(struct.pack('<i', val))
    body = bytes(body)
    header = build_header(kFlagHasChecksum, 4, 0) # 1 root + 3 children = 4
    footer = build_footer(body)
    return header + body + footer

def generate_array_with_ref():
    body = bytearray()
    body.append(kTypeArray)
    body.extend(encode_varint(2))
    # Child 1: int32
    body.append(kTypeInt32)
    body.extend(struct.pack('<i', 42))
    # Child 2: reference to index 1 (Child 1)
    body.append(kTypeReference)
    body.extend(encode_varint(1))
    body = bytes(body)
    header = build_header(kFlagHasChecksum, 3, 0) # 1 root + 2 children = 3
    footer = build_footer(body)
    return header + body + footer

def generate_nested_arrays():
    body = bytearray()
    body.append(kTypeArray)
    body.extend(encode_varint(1))
    # Child 1: inner array
    body.append(kTypeArray)
    body.extend(encode_varint(2))
    # 2 nulls
    body.append(kTypeNull)
    body.append(kTypeNull)
    body = bytes(body)
    header = build_header(kFlagHasChecksum, 4, 0) # root + inner + 2 nulls = 4
    footer = build_footer(body)
    return header + body + footer

def generate_map_with_keys():
    # String table with 1 key: "key1"
    st_entries = [b"key1"]
    st_body = bytearray()
    st_body.extend(encode_varint(len(st_entries)))
    for entry in st_entries:
        st_body.extend(encode_varint(len(entry)))
        st_body.extend(entry)
    st_body = bytes(st_body)
    
    body = bytearray()
    body.append(kTypeMap)
    body.extend(encode_varint(1)) # 1 pair
    body.extend(encode_varint(0)) # key index 0
    # Value: int32
    body.append(kTypeInt32)
    body.extend(struct.pack('<i', 100))
    body = bytes(body)
    
    st_offset = 16 + len(body)
    header = build_header(kFlagHasStringTable | kFlagHasChecksum, 2, st_offset)
    footer_data = body + st_body
    footer = build_footer(footer_data)
    return header + body + st_body + footer

def generate_typed_array_int16():
    body = bytearray()
    body.append(kTypeTypedArray)
    body.append(kTypeInt16)
    body.extend(encode_varint(4))
    for val in [1000, 2000, 3000, 4000]:
        body.extend(struct.pack('<h', val))
    body = bytes(body)
    header = build_header(kFlagHasChecksum, 5, 0) # 1 root + 4 children = 5
    footer = build_footer(body)
    return header + body + footer

def generate_string_element():
    body = bytearray()
    body.append(kTypeString)
    val = b"hello"
    body.extend(encode_varint(len(val)))
    body.extend(val)
    body = bytes(body)
    header = build_header(kFlagHasChecksum, 1, 0)
    footer = build_footer(body)
    return header + body + footer

def generate_blob_element():
    body = bytearray()
    body.append(kTypeBlob)
    val = b"world"
    body.extend(encode_varint(len(val)))
    body.extend(val)
    body = bytes(body)
    header = build_header(kFlagHasChecksum, 1, 0)
    footer = build_footer(body)
    return header + body + footer

def generate_compressed_int():
    # Uncompressed sub-document
    sub_body = bytes([kTypeInt32]) + struct.pack('<i', 999)
    sub_header = build_header(0, 1, 0) # no checksum, no string table
    uncompressed_data = sub_header + sub_body
    
    compressed_data = rle_compress(uncompressed_data)
    
    body = bytearray()
    body.append(kTypeCompressed)
    body.extend(encode_varint(len(uncompressed_data)))
    body.extend(encode_varint(len(compressed_data)))
    body.extend(compressed_data)
    body = bytes(body)
    
    header = build_header(kFlagHasChecksum, 2, 0) # root + compressed subroot = 2
    footer = build_footer(body)
    return header + body + footer

def generate_timestamp():
    body = bytearray()
    body.append(kTypeTimestamp)
    body.extend(struct.pack('<q', 1600000000))
    body = bytes(body)
    header = build_header(kFlagHasChecksum, 1, 0)
    footer = build_footer(body)
    return header + body + footer

def generate_nested_maps_with_ref():
    # String table with keys: "outer", "inner"
    st_entries = [b"outer", b"inner"]
    st_body = bytearray()
    st_body.extend(encode_varint(len(st_entries)))
    for entry in st_entries:
        st_body.extend(encode_varint(len(entry)))
        st_body.extend(entry)
    st_body = bytes(st_body)
    
    body = bytearray()
    # Map (root)
    body.append(kTypeMap)
    body.extend(encode_varint(1))
    body.extend(encode_varint(0)) # key "outer"
    # Value: inner Map
    body.append(kTypeMap)
    body.extend(encode_varint(1))
    body.extend(encode_varint(1)) # key "inner"
    # Value: reference to root Map (index 0)
    body.append(kTypeReference)
    body.extend(encode_varint(0))
    body = bytes(body)
    
    st_offset = 16 + len(body)
    header = build_header(kFlagHasStringTable | kFlagHasChecksum, 3, st_offset) # root + inner + ref = 3
    footer_data = body + st_body
    footer = build_footer(footer_data)
    return header + body + st_body + footer

def generate_array_near_cap():
    body = bytearray()
    body.append(kTypeArray)
    body.extend(encode_varint(22))
    for i in range(22):
        body.append(kTypeInt32)
        body.extend(struct.pack('<i', i))
    body = bytes(body)
    header = build_header(kFlagHasChecksum, 23, 0) # root + 22 = 23
    footer = build_footer(body)
    return header + body + footer

def generate_array_14_with_ref():
    body = bytearray()
    body.append(kTypeArray)
    body.extend(encode_varint(15)) # 14 int32s + 1 reference
    for i in range(14):
        body.append(kTypeInt32)
        body.extend(struct.pack('<i', i))
    body.append(kTypeReference)
    body.extend(encode_varint(1)) # points to first child
    body = bytes(body)
    header = build_header(kFlagHasChecksum, 16, 0) # root + 15 = 16
    footer = build_footer(body)
    return header + body + footer

def generate_ref_poolgrow_uaf():
    # Needs to end up with pool size = 24.
    # Root is Map. Children: 22 int32s, 1 reference.
    # Total nodes = 1 (root) + 22 (int32) + 1 (ref) = 24.
    # String table keys for the map. We need 23 keys.
    st_entries = [f"key{i}".encode('ascii') for i in range(23)]
    st_body = bytearray()
    st_body.extend(encode_varint(len(st_entries)))
    for entry in st_entries:
        st_body.extend(encode_varint(len(entry)))
        st_body.extend(entry)
    st_body = bytes(st_body)
    
    body = bytearray()
    body.append(kTypeMap)
    body.extend(encode_varint(23))
    # 22 int32 children
    for i in range(22):
        body.extend(encode_varint(i)) # key
        body.append(kTypeInt32)
        body.extend(struct.pack('<i', i))
    # 1 reference child pointing to index 0 (root Map)
    body.extend(encode_varint(22)) # key
    body.append(kTypeReference)
    body.extend(encode_varint(0)) # ref to root Map
    body = bytes(body)
    
    st_offset = 16 + len(body)
    header = build_header(kFlagHasStringTable | kFlagHasChecksum, 24, st_offset)
    footer_data = body + st_body
    footer = build_footer(footer_data)
    return header + body + st_body + footer

def main():
    seeds = {
        "minimal_null.bin": generate_minimal_null(),
        "array_int32.bin": generate_array_int32(),
        "array_with_ref.bin": generate_array_with_ref(),
        "nested_arrays.bin": generate_nested_arrays(),
        "map_with_keys.bin": generate_map_with_keys(),
        "typed_array_int16.bin": generate_typed_array_int16(),
        "string_element.bin": generate_string_element(),
        "blob_element.bin": generate_blob_element(),
        "compressed_int.bin": generate_compressed_int(),
        "timestamp.bin": generate_timestamp(),
        "nested_maps_with_ref.bin": generate_nested_maps_with_ref(),
        "array_near_cap.bin": generate_array_near_cap(),
        "array_14_with_ref.bin": generate_array_14_with_ref(),
        "ref_poolgrow_uaf.bin": generate_ref_poolgrow_uaf(),
    }
    
    base_dir = r"D:\projectXfenrir\helios_submission_repo\fuzz\corpus"
    targets = ["parser_fuzzer", "decoder_fuzzer", "container_fuzzer"]
    
    for name, content in seeds.items():
        for target in targets:
            if name == "ref_poolgrow_uaf.bin" and target != "parser_fuzzer":
                continue
            path = os.path.join(base_dir, target, name)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as f:
                f.write(content)
            print(f"Wrote {path} ({len(content)} bytes)")

if __name__ == "__main__":
    main()
