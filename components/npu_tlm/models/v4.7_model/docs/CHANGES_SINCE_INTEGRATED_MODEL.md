# Changes Since the Integrated Model

This document lists what differs between this package and the model revision that is currently built into the
system wrapper repository, and what an integrator has to expect when replacing one with the other. The release
notes (`RELEASE_NOTES.md`) describe the package as a whole; this document is the delta.

It covers the delivery path: `HasNpuTop` and the model root `core_rtl/`, that is, instructions pushed with extension
registers. The native model, which runs instructions pushed without extension registers, is no longer developed and
is not compared here.

Every number carries a label as in `VERIFICATION_REPORT.md`: **MEASURED** (from a run), **DERIVED** (computed from
measured numbers).

## 1. The model this document compares with

| Item | Value |
|---|---|
| Location in the system wrapper repository | `components/npu_tlm/models/v4.7_model` |
| How to recognise it | sha256 of its `package_manifest.txt` begins with `b48ae2f6d9d72740`; its `RELEASE_NOTES.md` ends section 2 with the item "The native model at revision v4.6" |
| Content | An earlier delivery of this model, and one local change of the wrapper repository to `has/gvu_obp.h` (negative output zero point of GEMM_FUSED, the same correction as in this package) |
| Wrapper used for the checks of this package | `components/npu_tlm` at revision `528007d`, built unchanged |

A tree that matches neither this description nor this package should be compared file by file with
`package_manifest.txt` of this package (`sha256sum -c package_manifest.txt` lists every file that differs).

## 2. What changes for an integrator

| Area | With this package | Action |
|---|---|---|
| Results of programs that use the extension registers | Unchanged, except tiles hit by the short-context error of the integrated model (section 3.1); those are now right | None. Rerun programs that have tiles with very small K (section 3.1) |
| Cycle counts | Core cycles unchanged; vector-unit stages follow the micro-architecture specification by default, so totals of programs with element-wise, softmax or LayerNorm work change (section 4) | `HAS_SPEC=v47ref` reproduces the cycle counts of the integrated model |
| Register interface | Six new extension registers (padding per side, padding value, dilation; section 5.1); the extension range ends at `0x40000500`; new error code `SWEEP` (11) | None for existing programs: registers not written keep the old behaviour. Instructions with too long a feeder sweep are refused (section 3.3). Firmware that uses the new registers or decodes error 11 needs their names in the platform's firmware register header (`INTEGRATOR_QUICK_START.md`, section 2.1) |
| Options off by default | Padding by the IFMAP feeder (`HAS_FEEDER_PAD`), the vector unit on its top level `GvuTop` (`HAS_GVU_TOP`) and cycle-level vector-unit pipelines (section 6) | None: while they are not set, results and cycles are those of sections 3 and 4 |
| Wrapper | No change needed to run: the wrapper maps its whole extension window onto the model, including the new registers. Its DRAM-window staging recognises an extended instruction by writes to `0x4000046C` to `0x400004E7`, while the model counts writes up to `0x400004FF`; every extended GEMM_FUSED writes registers of the first range, so no program depends on the difference | Replace the model tree, rebuild. Recommended: extend that range of the wrapper to `0x40000500` and add the new names to the wrapper's document (`INTEGRATOR_QUICK_START.md`, section 2.1) |
| Performance registers | `PERF_ACTIVE_PE_CYCLES`, `PERF_TOTAL_PE_CYCLES` and the related counters are now filled for instructions of `HasNpuTop` (section 5.3) | Software may read the PE utilization from them |
| Console output | Warning lines on stderr for refused instructions and for tiles or regions outside the hardware limits (section 5.4) | `SAURIA_CORE_RTL_WARN=0` silences them |

## 3. Changes that alter results

### 3.1 Short contexts in the RTL-ref core

| | Integrated model | This package |
|---|---|---|
| Tiles with a very small K (kernel positions x input channels of the core pass), mostly with 32 output channels per tile | The last context of the tile could receive no weights, so its output positions held the bias only; no error was reported. Example shapes: K = 8 with 2 contexts, K = 4 with 3, K = 2 with 5, K = 9 from 3 contexts, K = 3 from 5 contexts | Correct. The SAURIA RTL computes these shapes right; the model now follows it |
| K = 1 (configuration value `INCNTLIM` = 0) | Two multiply-accumulates per context | One, as in the RTL |
| Programs of the two reference networks | Not affected (no such tile) | Same results and cycles |

Evidence: `VERIFICATION_REPORT.md`, V32 (MEASURED: the RTL right on 12 shapes; without the correction 12 of 19
short-context layers of the check program wrong, with it 19 of 19 right). Details: `KNOWN_LIMITATIONS.md`, O23.

### 3.2 Other corrections of `HasNpuTop`

| Correction | Effect before | Effect now |
|---|---|---|
| Negative output zero point of GEMM_FUSED | Every output of the layer clamped to -128 (corrected locally in the integrated model as well) | Correct |
| A tile larger than the staging areas of `HasNpuTop` | Writes outside the DRAM vector, which could end the simulation with a memory error later | The tile is cut at the staging area and the instruction retires with internal errors; a warning names the tile |
| `--profile legacy` with `PAD_TAIL` (FLAGS bit 0) | Wrong results in the sequential schedule | Correct; the default profile was not affected |

### 3.3 Instructions now refused at the push

A GEMM_FUSED instruction whose tiles need a row sweep of the IFMAP feeder longer than 256 positions (large stride or
dilation with many positions per context, for example 3x3 stride 8 with 32 positions per context) ran in the
integrated model and gave wrong results without an error, or hung; the SAURIA RTL behaves the same. Such an
instruction is now refused with error `SWEEP` (11). Choose a smaller `Y_USED` (`KNOWN_LIMITATIONS.md`, O24). The two
reference networks are not affected.

## 4. Changes that alter cycle counts

The default configuration of the vector unit now follows the micro-architecture specification (V1.0): a 128 KB
double-buffered scratchpad, softmax and LayerNorm rows per group limited by the bank sizes, LUT tables of 256 / 512
entries. The core is unchanged.

| Workload | Integrated model | This package, default | This package, `HAS_SPEC=v47ref` | Label |
|---|---:|---:|---:|---|
| YOLOv8m, hardware as drawn | 62,681,721 | 62,666,123 | 62,681,721 | MEASURED |
| ViT-B/16, hardware as drawn | 41,065,258 | 41,680,289 | 41,065,258 | MEASURED |
| Transformer check program, vector-unit cycles | 250,885 | 285,205 | 250,885 | MEASURED |

The core itself takes the same cycles. Its busy counter, however, now counts 2 cycles more per core pass than the
counter of the integrated model (`SW_EVALUATION_GUIDE.md`, section 6): on YOLOv8m 55,653,442 against 55,638,052, which is
2 x 7,695 core passes (DERIVED from the two measured values). Further options that change cycles are off by default
(section 6).

## 5. Interface additions

### 5.1 Extension registers

| Index | Address | Through the wrapper (NPU base +) | Name | Meaning | Not written |
|---|---|---|---|---|---|
| 31 | `0x400004E8` | `0x104E8` | `PAD_VALUE` | Value of the padded positions, int8 in the low byte | 0 |
| 32 | `0x400004EC` | `0x104EC` | `PAD_T` | Padding rows above the input | All four sides use `PAD` |
| 33 | `0x400004F0` | `0x104F0` | `PAD_L` | Padding columns left of the input | As above |
| 34 | `0x400004F4` | `0x104F4` | `PAD_B` | Padding rows below the input | As above |
| 35 | `0x400004F8` | `0x104F8` | `PAD_R` | Padding columns right of the input | As above |
| 36 | `0x400004FC` | `0x104FC` | `DILATION` | Kernel taps `DILATION` positions apart; `(KW - 1) x DILATION` at most 63 | 1 |

All six apply to GEMM_FUSED, are one-shot like the other extension registers, and work with both ways of padding
(section 6). Writing any of `PAD_T` ... `PAD_R` replaces `PAD`; the unwritten sides are then 0. With these registers a
compiler leaves the input tensor unpadded in DRAM and does not build an im2col matrix: the hardware gathers the kernel
windows and inserts the padding value. Full rules: `SW_INTEGRATION_GUIDE.md`, section 3.3. Firmware names:
`tools/has/wrapper/fw/npu_has_fw.h` (`NPU_HAS_PAD_VALUE` ... `NPU_HAS_DILATION`).

The extension range of the model root `core_rtl/` grows from `0x400004FB` to `0x400004FF`. Older wrapper revisions
that need `tools/has/wrapper/npu_tlm_rich_window.patch` must apply the patch of this package, which has the new end.
Wrapper revisions that already have the DRAM window keep their own end of the range (`INTEGRATOR_QUICK_START.md`,
section 2.1, item 3).

### 5.2 Environment variables of the model root and of `HasNpuTop`

| Variable | Default | Purpose | Documented in |
|---|---|---|---|
| `SAURIA_CORE_RTL_RECORD=<directory>` | off | Record the program that reaches `HasNpuTop`, for a replay with `tools/has/tb_has_npu_top` | `SW_EVALUATION_GUIDE.md` |
| `SAURIA_CORE_RTL_REPORT=<file>` | off | One CSV line per retired instruction and a run summary (geometry, padding per side, padding value, dilation, cycles, core busy cycles, PE utilization, DMA and vector-unit cycles) | `SW_EVALUATION_GUIDE.md`, section 6.1 |
| `SAURIA_CORE_RTL_WARN=0` | warnings on | Silence the warning lines | `SW_EVALUATION_GUIDE.md` |
| `HAS_SPEC` | micro-architecture specification | `v47ref`: configuration of the integrated model; `rtl`, `mas10`: further groups | `HARDWARE_CONFIG.md`, section 6.3 |
| `HAS_FEEDER_PAD=1` | off | Padding generated by the IFMAP feeder (section 6) | `HARDWARE_CONFIG.md`, section 6.5 |
| `HAS_PAD_VALUE` | not set | Force one padding value for every convolution (experiments) | `HARDWARE_CONFIG.md`, section 6.5 |
| `HAS_SP_PRESET`, `HAS_SP_S` | specification | Scratchpad sizing | `HARDWARE_CONFIG.md` |
| `HAS_ELEM_RTL=1`, `HAS_OBP_RTL=1`, `HAS_ARITH_RTL=1`, `HAS_RANGE_PROBE=1`, `HAS_PSM_PROBE=1` | off | Cycle-level vector-unit pipelines and observation options | `GVU_RTL_CONFORMANCE.md`, section 4.10 |
| `HAS_GVU_TOP=1` or `2` | off | Vector-unit work of the instruction path on the top level `GvuTop` (section 6) | `GVU_TOP_CORE_INTEGRATION.md`, section 9 |
| `HAS_OBP_DUMP=<file>` | off | With `HAS_OBP_RTL=1`: write every activation table and epilogue beat to a file for `tools/has/tb_gvu_top_replay.cpp` | `GVU_TOP_CORE_INTEGRATION.md`, section 9.3 |

### 5.3 Performance registers through the wrapper

For every instruction of `HasNpuTop` the model root fills `PERF_ACTIVE_PE_CYCLES` (real multiply-accumulates),
`PERF_TOTAL_PE_CYCLES` (core busy cycles x 1,024), `PERF_SA_CYCLES`, `PERF_MAC_OPS` and the DMA port cycles, so the PE
utilization is the ratio of the first two (`SW_INTEGRATION_GUIDE.md`, section 10.3). Cycle counts are unchanged by it.

### 5.4 Checks at the push

The model root prints one warning line on stderr when an instruction is refused or retires with an internal error, when
a region of an instruction ends beyond the DRAM image area, and when one tile does not fit one buffer of SRAM-A, SRAM-B
or SRAM-C (80,896 bytes, 82,944 bytes, 24,576 partial sums; the input window is sized with the effective kernel of a
dilated layer). The instruction still runs; the report summary counts these cases (`SW_EVALUATION_GUIDE.md`, section
4.1).

## 6. Options that are off by default

| Option | What it changes | Results | Cycles | Label |
|---|---|---|---|---|
| `HAS_FEEDER_PAD=1` | SRAM-A holds only the part of a tile's input window inside the image; the IFMAP feeder inserts the padding value while it gathers. Four new core configuration registers (`ACT_PAD_*`), written by the data flow controller, not by software | Bit-exact with the default flow on every program checked, including whole YOLOv8m | +8 cycles per padded core pass; whole YOLOv8m 62,698,248 instead of 62,666,123 (+0.05 %); core busy equal on YOLOv8m, +8 cycles on one tile of the dilation check program | MEASURED |
| `HAS_ELEM_RTL=1`, `HAS_OBP_RTL=1` | Cycle-level max pool, residual add and requant / epilogue pipelines in the instruction path | Equal to the functional model on whole YOLOv8m | YOLOv8m 62,666,994 and 62,661,785 | MEASURED |
| `HAS_GVU_TOP=1` | GEMM epilogue (stream mode, depth 9) and residual add run on `GvuTop`; max pool on the cycle-level pipeline | Equal to the functional model on YOLOv8m instructions 86 to 88, a two-instruction window and the residual adds of both networks; whole networks not run | Equal to `HAS_OBP_RTL=1` on the instructions run (window 1,658,651) | MEASURED |
| `HAS_GVU_TOP=2` | As 1, and the activation table of a layer is charged as `GvuTop`'s path (DMA of 256 bytes plus 5 cycles) instead of 4,096 host-port cycles | As 1 | 4,087 fewer cycles per layer with an activation (window 1,650,477) | MEASURED |

The padding by the feeder is a proposal of this model to the hardware team; no hardware description exists for it
yet (`KNOWN_LIMITATIONS.md`, O21).

## 7. Tools and documents added

| Item | Purpose |
|---|---|
| `tools/has/ref_program.py` | Independent integer reference for a program of extended instructions (GEMM_FUSED with padding per side, padding value and dilation; ELEM_WISE ADD, MAX_POOL, AVG_POOL); compares with a RAM dump or a recording and names the first wrong instruction |
| `tools/has/make_ref_gate.py`, `tools/has/make_pad_gate.py` | Check programs for instruction forms the networks do not use: padding per side and value, random layers, short contexts, dilation (fixed and random) |
| `tools/has/regress_gvu.sh` | Regression of the vector-unit blocks, the instruction path, padding, short contexts and dilation |
| `tools/has/wrapper/fw/` | Firmware definitions in C (with the model's status register and error codes) and a program replay engine with CRC checks; the engine reads the model's status after every push and reports an instruction the model refused with its error code |
| `has/rtl_ref_gvu_*.h` and their tests | Vector-unit blocks ported from the hardware RTL, compared with an RTL simulation; not used by the instruction path unless an option above selects them |
| `docs/SW_EVALUATION_GUIDE.md` | Running, checking and comparing a network through the wrapper; reading the performance figures |
| `docs/IFMAP_FEEDER_PADDING.md` | Convolution padding in the IFMAP feeder at the level of an RTL implementation, for the hardware team |
| `docs/INTEGRATOR_QUICK_START.md` | Short path for the integrating team |
| `docs/GVU_RTL_CONFORMANCE.md`, `docs/GVU_PIPELINE_STEPS.md`, `docs/GVU_TOP_IMPLEMENTATION_AND_TEST.md` | Vector unit against its RTL and its specification |
| `has/rtl_ref_gvu_top.h` (`GvuTop`), its test benches and `target/test/`; `has/gvu_top_bridge.h`; `tools/has/tb_gvu_top_vs_model.cpp`, `tb_gvu_top_replay.cpp`; `docs/GVU_TOP_VERIFICATION_REPORT.md`, `docs/GVU_TOP_CORE_INTEGRATION.md` | Top level of the vector unit and its connection to the core; used by the instruction path only with `HAS_GVU_TOP` |
| `docs/CHANGES_SINCE_INTEGRATED_MODEL.md` | This document |

## 8. Replacing the integrated model

1. Replace `components/npu_tlm/models/v4.7_model` with the source tree of this package (data not needed for the
   wrapper build). Keep the wrapper sources as they are.
2. Rebuild the wrapper with the model root `core_rtl/` as before.
3. Check with the wrapper's own test and with `tools/has/wrapper/test_npu_tlm_core_rtl.cpp` on the transformer check
   program of the data package: 14 of 14 instructions, 285,205 cycles in the default configuration, 250,885 with
   `HAS_SPEC=v47ref` (MEASURED through wrapper revision `528007d`).
4. For programs of another compiler: record one run with `SAURIA_CORE_RTL_RECORD` and check it with
   `tools/has/ref_program.py` (`SW_EVALUATION_GUIDE.md`, section 5.1).

## 9. Points that did not change

These hold for the integrated model and for this package alike.

| Point | Consequence |
|---|---|
| A push that `HasNpuTop` refuses still completes in the wrapper's `STATUS` register; the refusal shows only in `RICH_STATUS` (alias `0x10318`, bit 2), in the warning line and in the report summary | Firmware that checks only `STATUS.DONE` does not see a refused instruction |
| Pushing an extended instruction while another runs, or writing into the DRAM window during a run, corrupts data without an error (`KNOWN_LIMITATIONS.md`, O18) | Wait for completion before the next push |
| Only the core is cycle-accurate; DMA, epilogue and vector-unit timing are modelled with parameters (`VERIFICATION_REPORT.md`, section 1.1) | Totals move when the hardware team fixes those parameters |

## 10. Files that differ

Comparison of this package with the integrated model tree, line endings ignored, `data/` and `package_manifest.txt`
left out: 79 files changed, 79 files or directories added, none removed.

| Area | Changed | Added |
|---|---|---|
| RTL-ref core | `rtl_ref_npu_top.h`, `rtl_ref_config_map.h`, `rtl_ref_config_regs.h`; `control/rtl_ref_defaults.h`, `rtl_ref_feeders_fsm.h`, `rtl_ref_main_controller.h`, `native_lane_a_core.h`; `data_feeder/rtl_ref_ifmap_feeder.h`, `rtl_ref_ifmap_feeder_rtl.h`, `rtl_ref_ifmap_idxcnt.h`, `rtl_ref_wei_feeder.h`, `rtl_ref_wei_feeder_rtl.h`, `rtl_ref_wei_idxcnt.h` | |
| `HasNpuTop` and the vector unit (`has/`) | `has_npu_top.h`, `has_mmio_compat.h`, `has_tile_iter.h`, `gvu_elemwise.h`, `gvu_fused_attn.h`, `gvu_layernorm.h`, `gvu_lsu.h`, `gvu_lut.h`, `gvu_obp.h`, `gvu_quant.h`, `gvu_softmax.h`, `HAS_IFACE.md`, `HAS_IFACE_RCE.md` | `has_pad_gen.h`, `gvu_pipeline_ctrl.h`, `gvu_psm_probe.h`, `gvu_range_probe.h`, `gvu_rtl_arith.h`, `gvu_rtl_elem.h`, `gvu_rtl_obp.h`, `gvu_sp_map.h`, `gvu_top_bridge.h`, `rtl_ref_gvu_ctrl.h`, `rtl_ref_gvu_elemwise.h`, `rtl_ref_gvu_fu.h`, `rtl_ref_gvu_lut.h`, `rtl_ref_gvu_requant.h`, `rtl_ref_gvu_scratchpad.h`, `rtl_ref_gvu_top.h`, `rtl_ref_gvu_vlsu.h`, `rtl_ref_gvu_vrf.h` |
| Model root `core_rtl/` | `npu_top.h`, `README.md` | 17 forwarding headers in `core_rtl/has/` for the new `has/` files |
| Wrapper aids `tools/has/wrapper/` | `npu_tlm_rich_window.patch`, `test_npu_tlm_core_rtl.cpp`, `fw/make_fw_program.py`, `fw/npu_has_fw.h`, `fw/npu_has_fw.c`, `fw/fw_main_example.c`, `fw/test_fw_replay.cpp` | |
| Tools and tests | `tools/has/tb_has_npu_top.cpp`, `tb_has_mmio_compat.cpp`, `tb_gvu_quant.cpp`; `tools/fe/fe_ref_gvu.py`, `fe_ref_int8.py`, `fe_step6b_export_net.py`, `fe_step6b_export_obp.py` | `tools/has/ref_program.py`, `make_pad_gate.py`, `make_ref_gate.py`, `mas_ref.py`, `regress_gvu.sh`, `check_rtl_trace.sh`, `measure_cfg_instr.sh`, `measure_mas_instr.sh`, `tb_has_pad_gen.cpp`, `tb_gvu_mas.cpp`, `tb_gvu_top.cpp`, `tb_gvu_vec_export.cpp`, `tb_gvu_top_vs_model.cpp`, `tb_gvu_top_replay.cpp`, `tb_rtl_ref_gvu_*.cpp` (10 files), `gvu_oracle/`, `gvu_top_vectors/`; `tools/trace_gvu_top.cpp`, `tools/gen_test_datasets.cpp`; `tb_gvu_top.cpp`, `tb_gvu_top_layernorm.cpp`, `tb_gvu_top_gemm_fused.cpp`, `tb_gvu_top_residual_add.cpp` and `target/test/` at the root |
| Documents | `docs/ARCHITECTURE.md`, `BUILD_AND_RUN.md`, `CODE_MAP.md`, `FRONTEND_GUIDE.md`, `HARDWARE_CONFIG.md`, `HAS_VALIDATION.md`, `INTERFACE_SPEC.md`, `KNOWN_LIMITATIONS.md`, `PERFORMANCE_REPORT.md`, `SW_INTEGRATION_GUIDE.md`, `TILING_GUIDE.md`, `USER_GUIDE.md`, `VERIFICATION_REPORT.md`; `README.md`, `RELEASE_NOTES.md` | `docs/CHANGES_SINCE_INTEGRATED_MODEL.md`, `GVU_PIPELINE_STEPS.md`, `GVU_RTL_CONFORMANCE.md`, `GVU_TOP_IMPLEMENTATION_AND_TEST.md`, `GVU_TOP_VERIFICATION_REPORT.md`, `GVU_TOP_CORE_INTEGRATION.md`, `IFMAP_FEEDER_PADDING.md`, `INTEGRATOR_QUICK_START.md`, `SW_EVALUATION_GUIDE.md` |
| Build files | `Makefile`, `.gitignore` | |
| Native model and its tests (not compared, see the start of this document) | `npu_top.h`, `config_map.h`, `config_regs.h`, `control/main_controller.h`, `psm/obp_top.h`, `psm/psm_top.h`, `psm/re_rce.h`, `tb_obp.cpp`, `tb_re.cpp`, `tb_unified_smoke.cpp`, `tools/fe/sysc/tb_fe_core_net.cpp`, `tb_fe_network.cpp`, `tb_obp_tiles.cpp`, `tools/test_rtl_ref_*_check.cpp` (6 files), `tools/test_sauria_ref_layer_runner.cpp` | `tb_pipeline_re.cpp`, `tools/obp_port_compat.h` |
