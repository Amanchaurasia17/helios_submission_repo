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
│   │   ├── diff_fuzzer/
│   │   └── parser_fuzzer/
│   ├── container_fuzzer.cc
│   ├── decoder_fuzzer.cc
│   ├── diff_fuzzer.cc
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
This produces four binaries: `parser_fuzzer`, `decoder_fuzzer`, `container_fuzzer`, and `diff_fuzzer`.

To clean the build artifacts:
```bash
make clean
```

To run a fuzzer locally with a seed corpus:
```bash
./parser_fuzzer fuzz/corpus/parser_fuzzer
```

The diff comparator target can be exercised with the same style of invocation:
```bash
./diff_fuzzer fuzz/corpus/diff_fuzzer
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

The library includes parser, decoder, document, and diffing logic so that fuzzing can cover both single-document and cross-document code paths.
