# Minimal Makefile for the graph serializer/deserializer
CXX ?= g++
CXXFLAGS ?= -O3 -std=gnu++17
BUILD_DIR = build
BIN := ${BUILD_DIR}/run

.PHONY: all build serialize deserialize check clean

all: build

build: $(BIN)

$(BIN): main.cpp
	mkdir -pv ${BUILD_DIR}
	$(CXX) $(CXXFLAGS) $< -o $@

# Usage: make serialize IN=input.tsv OUT=graph.bin
serialize: $(BIN)
	./$(BIN) -s -i $(IN) -o $(OUT)

# Usage: make deserialize IN=graph.bin OUT=output.tsv
deserialize: $(BIN)
	./$(BIN) -d -i $(IN) -o $(OUT)

# Usage: make check IN=input.tsv OUT=output.tsv
check:
	python3 check_edges.py $(IN) $(OUT)

clean:
	rm -f $(BIN) *.bin *.out.tsv
