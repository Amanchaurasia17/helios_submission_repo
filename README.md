# Helios — C++ FBF Serialization Library

Helios is a self-contained C++14 library implementing the Fenrir Binary Format (FBF), a binary serialization format supporting scalars, arrays, maps, references, and compression.

## Repository Layout
```text
helios_submission_repo/
├── .clusterfuzzlite/
│   ├── Dockerfile
│   ├── build.sh
│   └── project.yaml
├── fuzz/
│   ├── corpus/
│   │   ├── container_fuzzer/
│   │   ├── decoder_fuzzer/
│   │   └── parser_fuzzer/
│   ├── container_fuzzer.cc
│   ├── decoder_fuzzer.cc
│   └── parser_fuzzer.cc
├── include/
│   └── helios/
│       ├── helios.h
│       ├── helios_checksum.h
│       ├── helios_decoder.h
│       ├── helios_document.h
│       ├── helios_node.h
│       ├── helios_parser.h
│       ├── helios_string_table.h
│       ├── helios_types.h
│       └── helios_varint.h
├── src/
│   ├── helios_checksum.cc
│   ├── helios_decoder.cc
│   ├── helios_document.cc
│   ├── helios_node.cc
│   ├── helios_parser.cc
│   ├── helios_string_table.cc
│   └── helios_varint.cc
├── generate_seeds.py
├── fix.patch
└── Makefile
```

## Build Instructions

To build the fuzzing targets locally:
```bash
make
```
This produces three binaries: `parser_fuzzer`, `decoder_fuzzer`, and `container_fuzzer`.

To clean the build artifacts:
```bash
make clean
```

To run a fuzzer locally with a seed corpus:
```bash
./parser_fuzzer fuzz/corpus/parser_fuzzer
```

## Regenerating the Seed Corpus

The seed corpus files are generated programmatically:
```bash
python3 generate_seeds.py
```

## Applying the Fix Patch

To apply the fix patch and resolve all known bugs:
```bash
patch -p1 < fix.patch
```

## Known Bugs

### Bug 1: Out-of-bounds Read in FieldDecoder
An out-of-bounds read occurs in `FieldDecoder::decode_typed_array_field` within `src/helios_decoder.cc`. When copying typed array scalar payloads from a raw input buffer, the function fails to validate that the source child offset plus the requested byte count does not exceed the input boundary. A specially crafted input with a typed array structure located near the end of the buffer triggers an out-of-bounds read during the copy operation.

### Bug 2: Heap Buffer Overflow in StringTable
A heap buffer overflow write occurs in `StringTable::load` within `src/helios_string_table.cc`. During the first pass, the parser computes the total size for the string buffer but omits the space for the NUL terminator of the final entry. During the second pass, writing the NUL terminator for the last entry performs a one-byte write past the end of the allocated buffer.

### Bug 3: Use-After-Free in Document Resolver
A use-after-free read occurs in `Document::resolve_all_references` within `src/helios_document.cc`. When resolving a reference node to a container, the pool allocates a new node. If the allocation triggers a pool resize, the old nodes backing array is freed, leaving the target pointer dangling. The subsequent dereference of the target pointer to copy its fields triggers a use-after-free read.
