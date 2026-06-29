#!/bin/bash -eu

IFLAGS="-I$SRC/include"

$CXX $CXXFLAGS -std=c++14 -c $IFLAGS $SRC/src/helios_varint.cc      -o varint.o
$CXX $CXXFLAGS -std=c++14 -c $IFLAGS $SRC/src/helios_checksum.cc    -o checksum.o
$CXX $CXXFLAGS -std=c++14 -c $IFLAGS $SRC/src/helios_node.cc        -o node.o
$CXX $CXXFLAGS -std=c++14 -c $IFLAGS $SRC/src/helios_string_table.cc -o string_table.o
$CXX $CXXFLAGS -std=c++14 -c $IFLAGS $SRC/src/helios_parser.cc      -o parser.o
$CXX $CXXFLAGS -std=c++14 -c $IFLAGS $SRC/src/helios_decoder.cc     -o decoder.o
$CXX $CXXFLAGS -std=c++14 -c $IFLAGS $SRC/src/helios_document.cc    -o document.o
$CXX $CXXFLAGS -std=c++14 -c $IFLAGS $SRC/src/helios_debug.cc       -o debug.o

LIB_OBJS="varint.o checksum.o node.o string_table.o parser.o decoder.o document.o debug.o"

$CXX $CXXFLAGS -std=c++14 $LIB_FUZZING_ENGINE \
    $IFLAGS $SRC/fuzz/parser_fuzzer.cc $LIB_OBJS \
    -o $OUT/parser_fuzzer

$CXX $CXXFLAGS -std=c++14 $LIB_FUZZING_ENGINE \
    $IFLAGS $SRC/fuzz/decoder_fuzzer.cc $LIB_OBJS \
    -o $OUT/decoder_fuzzer

$CXX $CXXFLAGS -std=c++14 $LIB_FUZZING_ENGINE \
    $IFLAGS $SRC/fuzz/container_fuzzer.cc $LIB_OBJS \
    -o $OUT/container_fuzzer

if [ -d "$SRC/fuzz/corpus/parser_fuzzer" ]; then
    zip -j $OUT/parser_fuzzer_seed_corpus.zip \
        $SRC/fuzz/corpus/parser_fuzzer/*
fi
if [ -d "$SRC/fuzz/corpus/decoder_fuzzer" ]; then
    zip -j $OUT/decoder_fuzzer_seed_corpus.zip \
        $SRC/fuzz/corpus/decoder_fuzzer/*
fi
if [ -d "$SRC/fuzz/corpus/container_fuzzer" ]; then
    zip -j $OUT/container_fuzzer_seed_corpus.zip \
        $SRC/fuzz/corpus/container_fuzzer/*
fi
