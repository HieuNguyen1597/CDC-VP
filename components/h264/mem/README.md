# Memory Architecture (MEM)

**Owner:** Nguyen Dinh Nhat Nguyen
**Work scope:** Coding and Reconstruction  
**Specification:** SISLAB H.264/AVC Hardware Architecture Specification v2.1.11, Chapter 12 — Memory Architecture

## Overview

MEM provides byte-addressable internal and external storage for SystemC/TLM transactions. It is a functional memory model rather than a cycle-accurate SRAM/DDR controller.

## Source Files

- `include/memory_map.h`, `src/memory_map.cpp` — TLM target, address decoding and backing vectors.
- `include/internal_memory.h`, `src/internal_memory.cpp` — standalone internal-memory helper.
- `include/external_memory.h`, `src/external_memory.cpp` — standalone external-memory helper.
- `test/test_mem.cpp` — component test; `test/CMakeLists.txt` — test target.

## Address Map

| Region | Base address | Size |
|---|---:|---:|
| Internal | `0x00000000` | 64 KiB |
| External | `0x10000000` | 16 MiB |
| Other | — | Unmapped; returns an address error |

## Processing Flow

1. An initiator sends a TLM read/write payload.
2. `MemoryMap::b_transport()` validates the command, data pointer, transfer length, streaming width and optional byte enables.
3. The address decoder checks that the full transfer fits within one mapped region.
4. Enabled bytes are copied between the payload and the selected backing vector.
5. Successful transactions add 10 ns annotated delay; invalid transactions return a TLM error response.

## Build and Test

Run from the repository root:

```bash
cd mem/test
rm -rf build && mkdir build && cd build
cmake ..
make -j$(nproc)

./test_h264_vp_mem
```

## Limitations

The current storage is flat byte arrays without banks, arbitration, burst behavior, ECC, refresh or realistic memory timing. `MemoryMap` does not currently instantiate the standalone memory helpers or implement H.264-specific CMB/NAL/reference-picture allocation.
