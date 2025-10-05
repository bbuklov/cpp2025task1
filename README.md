# Graph Serializer/Deserializer (C++17)

## Overview
- Input TSV: `u<TAB>v<TAB>w` where `u,v` are `uint32` (0..2^32−1), `w` is `0..255`.
- Undirected graph; self-loops allowed; no multi-edges required in input.
- CLI:
  - Serialize: `./run -s -i input.tsv -o graph.bin`
  - Deserialize: `./run -d -i graph.bin -o output.tsv`
- Output TSV may differ by line order and by swapping `u`/`v` in a line (edge is undirected).

## Binary format (compact, LE, version 1)
- Header:
  - Magic `GRPH` (4B), `version=1` (1B), `endian=1` for little-endian (1B)
  - `N` (uint32), `M` (uint64) — vertices and edges total (loops included)
- Section A — mapping `newId → originalId`: `N * uint32`
- Section B — "upper" adjacency (CSR-like): for each vertex `i=0..N-1`
  - `deg_plus(i)` as VarUInt
  - For each neighbor `j>i` in ascending order:
    - `gap = j - prev` (VarUInt), `prev` starts at `i`
    - `weight` (1 byte)
- Section C — loops:
  - `L` as VarUInt, then `L` entries of `{ vertex_delta (VarUInt), weight (1 byte) }`, where `vertex_delta` is delta from previous loop vertex (start at 0).

## Build
```bash
g++ -O3 -std=gnu++17 run.cpp -o run
```

## Usage
```bash
# Serialize
./run -s -i input.tsv -o graph.bin

# Deserialize
./run -d -i graph.bin -o output.tsv
```

## Round-trip check (order-independent)
Use `check_edges.py`:
```bash
python3 check_edges.py input.tsv output.tsv  # prints match=True on success
```

## One button check

Use `one_button_check.sh`:
```bash
./one_button_check.sh
```

## Notes
- Single-threaded; uses `mmap` (or buffered read) and buffered write.
- Fast custom TSV parser; VarUInt encoder (LEB128-style).
- Memory footprint is O(N + E).
- Original task text is located at `task1.pdf`.
- Unpack assets.tar.gz with sample input data (`one_button_check.sh` expects assets directory to exist).
