# Entropy Encoder and NAL Hardware (EC)

**Owner:** Nguyen Dinh Nhat Nguyen  
**Work scope:** Coding and Reconstruction  
**Specification:** SISLAB H.264/AVC Hardware Architecture Specification v2.1.11, Chapter 10 — Entropy Encoder and NAL Hardware

## Overview

EC converts a block of signed quantized coefficients into a bitstream, appends RBSP trailing bits and formats the output as an Annex-B-style NAL unit.

## Source Files

- `include/ec_top.h`, `src/ec_top.cpp` — TLM registers, validation and dispatch.
- `include/cavlc.h`, `src/cavlc.cpp` — CAVLC coefficient coding.
- `include/exp_golomb.h`, `src/exp_golomb.cpp` — unsigned and signed Exp-Golomb helpers.
- `include/cabac.h`, `src/cabac.cpp` — simplified CABAC-named path.
- `include/syntax_builder.h`, `src/syntax_builder.cpp`; `include/slice_encoder.h`, `src/slice_encoder.cpp` — supported syntax and slice helpers.
- `include/nal_formatter.h`, `src/nal_formatter.cpp` — NAL header, start code and emulation prevention.
- `test/test_ec.cpp` — component test; `test/CMakeLists.txt` — test target.

## Processing Flow

1. Write 16 signed coefficient levels, QP and entropy-coding mode through the TLM registers.
2. Set the start bit in the control register.
3. `EcTop` validates the request, builds the supported syntax and dispatches the selected coding path.
4. The bit writer appends RBSP trailing bits; `NalFormatter` adds the NAL header/start code and emulation-prevention bytes.
5. Read status, output buffer and output length. The reported length includes 4-byte DMA padding.

## Algorithm Notes

- CAVLC processes non-zero coefficients and trailing ones, then encodes coefficient tokens, levels, total zeros and runs.
- Exp-Golomb helpers encode unsigned and signed syntax values.
- NAL formatting inserts an emulation-prevention byte `0x03` after two zero bytes when the following RBSP byte is in the range `0x00`–`0x03`.

## Build and Test

Run from the repository root:

```bash
cd ec/test
rm -rf build && mkdir build && cd build
cmake ..
make -j$(nproc)

./test_h264_vp_ec
```

## Limitations

`Cabac::encode()` is not a normative H.264 CABAC arithmetic encoder. The syntax builder supports only a subset of configurations. Validate generated bitstreams with independent reference vectors/decoders before claiming full H.264/AVC conformance.
