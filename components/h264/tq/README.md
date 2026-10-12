# Transform and Quantization Hardware (TQ)

**Owner:** Nguyen Dinh Nhat Nguyen
**Work scope:** Coding and Reconstruction  
**Specification:** SISLAB H.264/AVC Hardware Architecture Specification v2.1.11, Chapter 9 — Transform and Quantization Hardware

## Overview

TQ models the transform-and-quantization stage for a 4×4 residual block. Its current flow performs forward transform and quantization, then inverse scaling/transform and predictor reconstruction.

## Source Files

- `include/tq_top.h`, `src/tq_top.cpp` — TLM target, register handling, validation and dispatch.
- `include/ftq.h`, `src/ftq.cpp` — forward transform and quantization.
- `include/itq.h`, `src/itq.cpp` — inverse scaling/transform and reconstruction.
- `include/transpose_ram.h`, `src/transpose_ram.cpp` — intermediate transform storage model.
- `test/test_tq.cpp` — component test; `test/CMakeLists.txt` — test target.

## Processing Flow

1. Write the residual block, predictor samples and QP through the TLM registers.
2. Set the start bit in the control register.
3. `TqTop` validates the request and invokes the forward transform/quantization path.
4. `Ftq` performs row/column transform passes and QP-dependent quantization.
5. `Itq` inverse-scales and transforms the levels, adds the predictor and clips reconstructed samples to `[0, 255]`.
6. Read the quantized levels, reconstructed samples and status registers.

## Algorithm Notes

- Quantization validates QP in the range 0–51 and uses `QP % 6`, `QP / 6`, scaling factors, rounding and sign preservation.
- The implementation contains luma/chroma DC helper routines, but the top-level start path does not automatically dispatch every specialized DC path.
- `TransposeRam` models intermediate storage; it is not a cycle-accurate SRAM implementation.

## Build and Test

Run from the repository root:

```bash
cd tq/test
rm -rf build && mkdir build && cd build
cmake ..
make -j$(nproc)

./test_h264_vp_tq
```

## Limitations

This is a functional SystemC/TLM block for a common 4×4 path, not a complete macroblock/frame pipeline. Passing component tests alone does not establish full H.264/AVC bit-exact conformance.
