# Deblocking-Filter Hardware (DF)

**Owner:** Nguyen Dinh Nhat Nguyen
**Work scope:** Coding and Reconstruction  
**Specification:** SISLAB H.264/AVC Hardware Architecture Specification v2.1.11, Chapter 11 — Deblocking-Filter Hardware

## Overview

DF models filtering across one edge between adjacent 4×4 blocks. Line memory retains the previous/left block, while the request provides the current/right block and filtering parameters.

## Source Files

- `include/df_top.h`, `src/df_top.cpp` — TLM registers, validation and dispatch.
- `include/df_filter.h`, `src/df_filter.cpp` — threshold selection, boundary-strength helper and filtering.
- `include/df_line_mem.h`, `src/df_line_mem.cpp` — previous-block storage.
- `include/df_types.h` — shared DF data types.
- `test/test_df.cpp` — component test; `test/CMakeLists.txt` — test target.

## Processing Flow

1. Optionally preload the left block and write the current/right block.
2. Configure boundary strength, QP, chroma mode and alpha/beta offsets.
3. Set the start bit in the control register.
4. `DfTop` loads the left block and calls `DfFilter` to evaluate and filter samples across the edge.
5. Read status and the filtered left/right blocks.

## Algorithm Notes

- Boundary strength `bs = 0` skips filtering; `bs = 4` selects the strong-filter branch; strengths 1–3 use `tc0`-based clipping.
- Alpha and beta indices are clipped to 0–51. Filtering proceeds only when the alpha and beta edge tests pass.
- A helper can derive boundary strength from block metadata, but `DfTop` currently accepts boundary strength directly from the request.

## Build and Test

Run from the repository root:

```bash
cd df/test
rm -rf build && mkdir build && cd build
cmake ..
make -j$(nproc)

./test_h264_vp_df
```

## Limitations

One edge is processed per start command; a frame-level scheduler is not included. Full normative validation still requires reference vectors for luma/chroma branches, boundary-strength derivation and filtering order.
