CXX      ?= clang++
CXXFLAGS ?= -std=c++14 -fsanitize=address,undefined -g -I include

SRCS = src/helios_varint.cc src/helios_checksum.cc src/helios_node.cc \
       src/helios_string_table.cc src/helios_parser.cc \
	src/helios_decoder.cc src/helios_document.cc src/helios_diff.cc \
	src/helios_debug.cc src/helios_chartables.cc

OBJS = $(SRCS:.cc=.o)

LIB_FUZZING_ENGINE ?= -fsanitize=fuzzer

all: parser_fuzzer decoder_fuzzer container_fuzzer diff_fuzzer

%.o: %.cc
	$(CXX) $(CXXFLAGS) -c $< -o $@

parser_fuzzer: fuzz/parser_fuzzer.cc $(OBJS)
	$(CXX) $(CXXFLAGS) $(LIB_FUZZING_ENGINE) $^ -o $@

decoder_fuzzer: fuzz/decoder_fuzzer.cc $(OBJS)
	$(CXX) $(CXXFLAGS) $(LIB_FUZZING_ENGINE) $^ -o $@

container_fuzzer: fuzz/container_fuzzer.cc $(OBJS)
	$(CXX) $(CXXFLAGS) $(LIB_FUZZING_ENGINE) $^ -o $@

diff_fuzzer: fuzz/diff_fuzzer.cc $(OBJS)
	$(CXX) $(CXXFLAGS) $(LIB_FUZZING_ENGINE) $^ -o $@

clean:
	rm -f $(OBJS) parser_fuzzer decoder_fuzzer container_fuzzer diff_fuzzer
