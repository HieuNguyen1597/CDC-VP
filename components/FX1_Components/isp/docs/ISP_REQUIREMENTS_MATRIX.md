# FX1 ISP VP — Requirement-to-test matrix

Status: **PASS** (test exists and passes), **PARTIAL** (part of the
requirement covered; the rest belongs to a later milestone), **TODO**,
**GAP** (cannot be implemented with the current map).

Tests:
- `U` = `tests/unit/test_control_unit.cpp` (CTest `fx1_isp.unit.control`)
- `I` = `tests/integration/test_csr_tlm.cpp` (CTest `fx1_isp.integration.csr_tlm`)
- `R` = `tests/unit/test_dma_rules.cpp` (CTest `fx1_isp.unit.dma_rules`)
- `D` = `tests/integration/test_dma_tlm.cpp` (CTest `fx1_isp.integration.dma_tlm`)
- `G` = `tools/gen_csr.py --check`, `tools/check_csr_header.py` (CTest `fx1_isp.csr.*`, enabled with `-DFX1_ISP_CSR_XLSX=…`)
- `W` = `tests/unit/test_row_stage.cpp` (CTest `fx1_isp.unit.row_stage`)
- `V` = `tests/unit/test_pipeline_vectors.cpp` over `tests/data/vectors` (CTest `fx1_isp.unit.pipeline_vectors`): bit-exact against the Python reference
- `B` = `tests/unit/test_block_vectors.cpp` over `tests/data/blocks` (CTest `fx1_isp.unit.block_vectors`)
- `S` = `tests/unit/test_stats.cpp` (CTest `fx1_isp.unit.stats`): directed, analytic or differential expectations
- `E` = `tests/integration/test_e2e_tlm.cpp` (CTest `fx1_isp.integration.e2e_tlm`): every vector through CSR + IDMA + pipeline + ODMA over TLM, statistics read over the CSR socket
- `K` = `fx1_isp_run_raw --synthetic` on `tests/data/4k/*` (CTest `fx1_isp.integration.e2e_4k.*`)
- `C` = `tests/integration/test_cont_tlm.cpp` on `tests/data/sequences/cont_100` (CTest `fx1_isp.integration.cont_tlm`)
- `X` = `tools/run_regression.py` (CTest `fx1_isp.regression.raw`, enabled with `-DFX1_ISP_RAW_INVENTORY=…`)

Status as of 2026-10-01, after M5. M4 evidence in detail:
[ISP_M4_QUALIFICATION.md](ISP_M4_QUALIFICATION.md). Per-register view:
[ISP_CSR_TRACEABILITY.md](ISP_CSR_TRACEABILITY.md). Acceptance evidence:
[evidence/README.md](evidence/README.md).

## G-CSR — register map and access semantics

| Req | Source | Test | Status |
|---|---|---|---|
| 226 registers / 298 fields, offsets, masks and resets match the spreadsheet | CSR XLSX | G (generator self-check + independent cross-check, 1346 values), U `test_table_shape` | PASS |
| Reset values, independent hand-copied spot checks (49 registers) | CSR XLSX | U `test_literal_resets` | PASS |
| RW/RO/W1C/W1S/W1SC behaviour for every register, all-ones and all-zeros writes | HAS Tab. 8-1 | U `test_access_sweep` | PASS (the 8 registers with read hooks or soft-reset side effects are covered by dedicated tests) |
| Unmapped read 0 / write ignored (all 16 158 unmapped words); reserved bits read 0; RO ignores writes | HAS §8, Tab. 7-10 | U `test_unmapped_and_reserved`, I | PASS |
| Byte strobes, no-strobe write, address bits [1:0] ignored | HAS Tab. 7-10 | U `test_strobes_and_alias`, I `test_identity_and_subword` | PASS |
| Sub-word TLM accesses, cross-word / burst / out-of-aperture responses | CSR-23 | I `test_identity_and_subword`, `test_decode_errors` | PASS |
| Hardware set wins over a same-cycle W1C; per-bit W1C | HAS Tab. 7-10 | U `test_w1c_semantics` | PASS |
| Level-held `IDMA_UNDERRUN` | HAS §6.25.8.3, CSR-06 | U `test_level_hold` | PASS (mechanism; source from M2) |
| Bayer encoding DEC-11 in header | DEC-11 | G (override assertion), U `test_resizer_geometry` | PASS |
| Tied-zero reserved bits | SPEC-05 | U `test_w1c_semantics`, `test_irq_aggregation` | PASS |
| Gamma / EE LUT ports, GTM read-back | CSR rows 190–272, CSR-21 | U `test_gamma_lut_port`, `test_ee_lut_port` | PASS |
| `RESIZER_OUT_W/H` derivation, including BGGR 2688×1520 → 2686×1518 | HAS §6.6.5.3, §6.20.10, CSR-09 | U `test_resizer_geometry` | PASS |
| Resizer configuration controller: E0–E35 publication of a stable key, pass-through on the next edge, stale result never published, SOF on the publication edge sees the previous set, reset set and re-derivation after a COMMON soft reset, DMA reset unaffected, `updated` no effect | MAS §6.20.4.3–4, Table 123–124 | U `test_resizer_publication` | PASS |
| Resizer modes while streaming: ODMA follows each frame's size without re-arm; a mode written during a frame applies to the next; a frame wider than the stride checked at arm refused, nothing written | MAS §6.20.4.4, §9.2, U4-A2 | D `test_resizer_switch` | PASS |
| `frame_start` pending flag, `frame_done` clear, no-hardware W1S | DEC-16, DEC-17, CSR-07 | U `test_frame_start_and_done` | PASS (SOF source from M2) |
| Block command registers delivered to hooks | M1 boundary | U `test_commands_recorded` | PASS (behaviour: M2/M3) |
| CSR access latency annotation | DEC-06 | I `test_latency` | PASS |
| Access with annotated delay takes effect at its annotated time (write: IRQ/status unchanged mid-delay, edge exactly at t0+delay; read: sees a hardware event inside the delay) | Review R1, DEC-06 | I `test_annotated_delay` | PASS |
| Pending commands discarded by external and soft reset | Review R2, DEC-14 | U `test_commands_cleared_on_reset`, I `test_external_reset_mid_run` | PASS |
| Generated artefacts carry no spreadsheet prose | Source confidentiality (Review R3) | Leak scan against CSR/HAS text (manual, 2026-09-30) | PASS |

## G-IRQ

| Req | Source | Test | Status |
|---|---|---|---|
| `irq` = OR of both groups after masking; latched when masked; per-bit clear | HAS §9.2 | U `test_irq_aggregation`, I `test_irq_pin` | PASS |
| Error aggregation into `COMMON_STATUS.error` / `error_irq` for all 9 sources | DEC-15 | U `test_error_aggregation`, I `test_irq_pin` | PASS |
| `stats_ready_irq` from AEC/AWB/AF done | HAS Tab. 9-2 | U `test_stats_ready`, S `test_frame_id_abort_irq` | PASS |
| IRQ edge count, no glitches | HAS §9.2 | I `test_irq_pin` | PASS |
| DMA sources: IDMA_START/DONE/UNDERRUN, ODMA_DONE/OVERFLOW, AXI_ERROR; `frame_done_irq` | HAS §6.25.11 | D (all tests) | PASS |
| `ODMA_DONE` IRQ edge not before the last write response | HAS Table 6-101 | D `test_single_frame` | PASS |
| Statistics sources (AEC/AWB/AF): one event per publication, also with an uncleared done bit | HAS §9.5, M3-A4 | S `test_frame_id_abort_irq`, V/E (done bits) | PASS |

## G-RESET

| Req | Source | Test | Status |
|---|---|---|---|
| External reset: registers, holds, LUT poison pattern (contents undefined), `irq` low | MAS §6.5.2, Table 183 | U `test_external_reset`, I `test_external_reset_mid_run` | PASS |
| CSR writes ignored while `rst_n` is low | Model contract | I start of `run()` | PASS |
| Soft reset scopes: COMMON (pipeline + DMA) and DMA-only preserved/cleared sets; pending AEC commit kept by both; the abandoned frame leaves no block or AEC/AF busy flag set, also with no re-arm | MAS Table 183 p316-317, §9.1.1, owner Q4, U1-R1 | U `test_soft_reset` ×2, `test_aec_commit_across_soft_reset`, `test_commands_cleared_on_reset`; S `test_resets`; C G-CONT reset bursts (idle after the pulse and one frame time later) | PASS |
| 15-cycle W1SC pulse, restart on rewrite, W1S/W1C held off, RW accepted | MAS §6.5.3 | U `test_soft_reset`, I `test_soft_reset_window` | PASS |
| Soft reset mid-frame: no memory access after the reset, ownership cleared, no late DONE, enables and counters kept, retry succeeds | DEC-14, HAS Tab. 9-1 | D `test_soft_reset_mid_frame` | PASS |
| External reset mid-frame: no access after reset, registers reset, re-program and run | HAS §6.5 | D `test_external_reset_mid_frame` | PASS |

## G-PORT

| Req | Test | Status |
|---|---|---|
| Standalone build with no dependency on a platform, a CPU or `components/isp_tlm` | CMake standalone configure; build with `-Wall -Wextra -Wpedantic -Wconversion -Wshadow` and no warnings | PASS |
| All sockets, reset and IRQ bindable | I (full bench binds every port) | PASS |
| Public interface guide | `docs/ISP_MODEL_INTERFACE.md` | PASS (updated at each milestone) |

## Specification review (M0)

| Item | Status |
|---|---|
| Control / CSR / reset / IRQ / DMA contract review (CSR-01..23) | Done for M1 scope; CSR-19/20 OPEN for M2 |
| P00-1: DOCX vs PDF comparison | OPEN |
| P02-ALG: per-block arithmetic review (LSC … AF) | Done (sheets ALG_A..F, decisions log §5) |

## G-DMA — DMA engines and frame lifecycle (M2)

| Req | Source | Test | Status |
|---|---|---|---|
| Burst formation: MAX_BURST beats, 4 KB boundary, partial final beat, no merge across lines | HAS Tab. 6-92, §6.25.13 | R `test_split_line`, D `test_burst_formation` | PASS |
| Start checks: base/stride multiple of B, IDMA stride ≥ 2W, ODMA strides ≥ W, W/H even and in range; refusal = ALIGN + `IRQ_CFG_ERROR`, no access | HAS Tab. 6-89, 6-100, MAS Table 161–162, SPEC-05 → §9 | R `test_setup_checks`, D `test_alignment_rejection` (11 cases, CFG_ERROR asserted) | PASS |
| Single frame: content, strides, only active bytes written, padding and guards untouched, counters, ownership, IRQs, busy/frame_done, frame_start cleared | HAS §6.25.5, §6.25.8–9, MAS Table 159, DEC-16/17 | D `test_single_frame` | PASS |
| DEC-04 geometry BGGR 2688×1520 → NV12 2686×1518, strides 5376/2688 | HAS Tab. 6-17 | D `test_real_geometry_bggr` | PASS (stub pixels) |
| Resizer output geometry through the ODMA (1280×720 → 640×360) | HAS §6.20.10 | D `test_resizer_scaling` | PASS (geometry; nearest stub, not the HAS scaler) |
| 4-buffer rotation, recycle order DONE→FREE, frame order kept | HAS §6.25.10 | D `test_rotation_and_recycle` | PASS |
| Underrun: level held, rising-edge IRQ, cleared only after the condition drops | HAS §6.25.8.3, CSR-06, DEC-15 | D `test_underrun` | PASS |
| Overflow: lossless stall, back-pressure to the IDMA, status held while stalled, IRQ once per episode | DEC-18, MAS Table 161, U5-A5 | D `test_overflow_lossless` | PASS |
| Frame metadata: tag/timestamp follow the frame across out-of-step rotations; snapshot at frame start; `FREE` cleared at frame start; exact SOF/EOF edges; nothing published for aborted/failed frames; reset retention | MAS §6.25.4.1.0.3 Table 159–160, U5-A2..A4 | D `test_metadata_rotation`, `test_metadata_snapshot_and_points`, `test_metadata_resets`, `test_axi_read_error`, `test_axi_write_error`; DRV `test_metadata_driver` | PASS |
| `FRAME_COUNTER`: core clocks since `i_rst_n` release, read edge exact, 0 in reset, kept by soft resets, wraps modulo 2^32 with no clock events | MAS Table 160, U5-A1 | U `test_time_base`; D `test_frame_counter`, `test_frame_counter_wrap` | PASS |
| ISP_EN ordering trap reported as ALIGN + `IRQ_CFG_ERROR`; recovery | DEC-20, MAS Table 161 | D `test_isp_en_trap` | PASS |
| AXI read error: VALID released, rotation kept, no DONE, output buffer still FREE, re-arm works | DEC-19 | D `test_axi_read_error` | PASS |
| AXI write error: no DONE, FREE cleared, rotation kept, no further write of that frame, IDMA unaffected | DEC-19 | D `test_axi_write_error` | PASS |
| 64-bit address registers on a 40-bit port | M2-A3 | R `test_port_address`, D `test_address_width` | PASS |
| IDMA_EN cleared mid-frame takes effect at the frame boundary | M2-A2 | D `test_idma_disable_at_frame_boundary` | PASS |
| ODMA armed then disabled: no new frame starts, no overflow report, frame kept and completed on re-enable | M2-A2, M2-A6, review M2-R1 | D `test_odma_disabled_after_arm` | PASS |
| Reset wakes engines in timed waits: a re-armed frame is accepted right after the soft-reset window | DEC-14, review M2-R2 | D `test_reset_wakes_line_wait` | PASS |
| Several outstanding transactions, AXI IDs, round-robin between Y and UV bursts | HAS §7.1.6, §6.25.9.2 | — | Not modelled (M2-A10) |

## G-IMAGE — pixel pipeline (M3, MR)

All rows: 25 vectors, comparison of the NV12 output byte for byte with the
Python reference, run at unit level (V) and through the whole IP (E).

| Req | Source | Test | Status |
|---|---|---|---|
| Input Formatter phase crop and 12-bit mask, all four Bayer orders | HAS §6.6, DEC-11, CSR-10 | V/E (RGGB, GRBG, GBRG, BGGR vectors) | PASS |
| BLC pedestal, per-channel trim, range scale | HAS §6.7 | V/E `full_bggr_66x34`, `full_chain_*` | PASS |
| LSC load sequencer, 3 profiles, bilinear mesh, strength > 1, overflow counters; mesh 2..128 nodes per axis and not above the active size; begin into the active profile rejected; begin invalidates the destination, abort or count mismatch leaves it invalid; geometry change invalidates all; boot via profile 1; selection re-checked at the accepted SOF (invalid → previous profile kept, `PROFILE_SEL_REJECT`) | HAS §6.8, MAS §6.8.3–6.8.4 Table 31, Figure 29, DEC-31 → §9, U1-R2 | V/E `lsc_*` (3), `edge_*`, U `test_lsc_loader`, `test_lsc_resize_during_load`, `test_lsc_selection_at_sof` | PASS |
| BPC spatial detection and correction; counters cleared at every SOF, counted in an enabled frame, 14-bit saturating, 0 after a disabled frame; `dynamic_det_en` no effect | MAS §6.9, DEC-28 → §9 | V/E `bpc_*` (incl. `bpc_dynamic_requested_64x32`), S `test_bpc_counters` | PASS |
| WB, DG | HAS §6.10–6.11 | V/E | PASS |
| Demosaic, reflect-101 borders | HAS §6.12 | V/E (all vectors), W | PASS |
| CCM sampled at SOF, `updated` without effect | Owner Q1, CSR/MAS 07/10 | V/E `gbrg_ccm_gate_64x36`, `full_bggr_66x34`; U `test_commit_gates` | PASS |
| Gamma, live table | HAS §6.14 | V/E | PASS |
| CSC BT.601 / BT.709 | HAS §6.15, SPEC-03 | V/E | PASS |
| GTM auto (bank swap at SOF, curve built at EOF) and manual LUT; table writes only while `EN` = 0 | HAS §6.16, MAS p146, ALG-GTM-* | V/E `gtm_*` (3-frame vector), U `test_gtm_lut_gate`, D GTM load while enabled | PASS |
| 2DNR, variance carried to the next frame | HAS §6.17, ALG-2DNR-* | V/E `nr2d_*`, B `nr2d_frames`, `nr_variance` | PASS |
| EE gain chain, radial gains | HAS §6.18 | V/E `ee_features_96x64`, B `ee_gain_chain`, `ee_frames` | PASS |
| EE three resident profiles: writes by `profile_wr_sel` (code 3 ignored), readback, `profile_sel` at SOF (code 3 = 0), scalars and enable frame-held, tables live (tearing), partial-write merge | MAS §6.18.4.1, §9.2 | U `test_ee_profiles`; P `test_three_profiles`, `test_frame_coherence`, `test_live_table_tearing`; V/E `ee_profiles_3frames_64x48`; C G-CONT EE deltas; D `test_ee_driver` | PASS |
| EE radial error: once per processed frame (EE on, ≥ 5 × 5, radial on, gain of the frame's profile > 1.5), W1C, recurs while programmed | MAS §6.18.4.1, §9.2 | U `test_ee_radial_error` | PASS |
| CNF sampled at SOF, inclusive thresholds | Owner Q1, CSR/MAS 07/10 | V/E `cnf_64x48`, B `cnf_frames` | PASS |
| Resizer: decimation stage count (DEC-23), bilinear, all scale codes | HAS §6.20, DEC-23 | V/E `resize_*` (3), V resizer plan table (160 rows) | PASS |
| Output Formatter NV12 | HAS §6.24 | V/E | PASS |
| Real camera RAW (DEC-04) through the pipeline | DEC-03/04 | X: 28 RAW × 4 profiles plus 4 real sequences, every frame bit-exact | PASS (see G-E2E) |
| Accuracy against an owner golden | DEC-05 | — | GAP (no owner golden) |

## G-CONFIG — configuration timing (M3)

| Req | Source | Test | Status |
|---|---|---|---|
| Snapshot at accepted SOF; gated sets commit immediately when idle, else at SOF | HAS §9.4.3, DEC-24/26/27 | U commit-gate tests, V/E | PASS |
| Live Gamma and EE tables; EE scalars sampled at SOF | DEC-25 → §9, MAS §6.18.4.1 | B `ee_frames` (written through the control unit), P `test_live_table_tearing` | PASS |
| AEC shadow + commit at SOF; AWB set and AF enable sampled at SOF | ALG-STAT-05, MAS §6.22.4.1, §6.23.2 | S `test_aec_commit`, `test_awb_sampled_at_sof`, `test_af_enable`, V/E `stats_commit_sequence_4frames_64x48` | PASS |
| Between-frame CSR writes (vector format `frames.csrw`) | — | V/E | PASS |

## G-STATS — AEC / AWB / AF (M3)

| Req | Source | Test | Status |
|---|---|---|---|
| AEC: tap post-BPC, channel classes, inclusive clip, strict OE/UE, unqualified green min/max/histogram, last zone absorbs the remainder | HAS §6.21, ALG-AEC-01/07 | V/E `stats_*` (6 vectors, readout through every mux) | PASS |
| AEC illegal grid → global and histogram only; reads beyond the grid are empty; every zone untouched (min 4095) after a reset | ALG-AEC-02/03, MAS §6.21.4.3 | V/E `stats_illegal_grids_64x32`, `stats_reset_grids_*`, S `test_resets` | PASS |
| Field widths and wrap: 33-bit sums, 21/16/22-bit counts, 16-bit zone count wrap | HAS Tab. 6-71 | V/E `stats_zone_wrap_512x512`, S `test_readout_widths` (incl. 4K all-4095 sum) | PASS |
| AWB: tap post-Demosaic, strict qualification, floor zone boundaries; illegal grid (0, > 64 / 32, more zones than samples) not accumulated or published, reported | MAS §6.22.3, ALG-AWB-01..04 | V/E `stats_*`, S `test_awb_grid` | PASS |
| AWB result banks: FREE → WRITING → READY → LEASED; oldest READY acquired; one lease; reads without a lease 0; leased bank stable while the other bank measures (AWB-LEASE) | MAS §6.22.4.2–3, Table 140 | S `test_single_memory_partial_reads`, `test_awb_full_and_order`, `test_context_latch`; D `check_awb` (every vector), `test_awb_driver` | PASS |
| AWB-FULL: no FREE bank at an enabled SOF → measurement skipped, `overrun`, `DROP_COUNT` +1 per frame, video continues; admission resumes after release | MAS §6.22.5.5 | S `test_awb_full_and_order`, `test_awb_reset_scopes`; D `test_awb_driver`; C G-CONT bursts of 3 | PASS |
| AWB-COMMAND: no-ops, RELEASE wins, byte strobes, commands read 0, readable one clock after ACQUIRE, zone word one clock after `AWB_ZONE_ADDR`, following-edge eligibility of publish/acquire and release/SOF | MAS §6.22.4.3, user decision 2026-10-09 | S `test_awb_commands` | PASS |
| Driver honours the MAS clocks with CSR accesses shorter than a core clock (ACQUIRE, RELEASE, result-memory address); lost lease drops the compatibility cache | MAS §6.21.5.1, §6.22.4.3, review U2-R1/R2 | D `driver_tlm_fast_csr`, `test_awb_driver` | PASS |
| AWB-REPORT: `clear_report` clears only `overrun`/`DROP_COUNT`, a same-edge loss survives it; `stat_done` clear releases nothing | MAS §6.22.4.3 | S `test_awb_report` | PASS (saturation at 2^32 − 1 by inspection) |
| AWB-RESET: DMA-only reset aborts the measurement in flight, keeps READY/LEASED and reporting; COMMON and hard reset clear ownership and reporting | MAS p248 | S `test_awb_reset_scopes`, `test_frame_id_abort_irq`; D `test_awb_driver` (lease ended by a reset) | PASS |
| AF: Sobel on CSC luma before GTM, replicate borders, ceil zone boundaries; EN sampled at SOF (a frame started enabled is published, one started disabled is ignored); `score_valid` with every publication | MAS §6.23, ALG-AF-03, U2-A2 | V/E, S `test_af_enable` | PASS |
| AEC single in-place memory: from an enabled SOF, written zones read their partial sums and unwritten zones the empty value; histogram partial; checked after every row and over TLM mid-frame; globals, AF scores and IDs keep the previous publication until EOF | DEC-29/30, review M3-R5 | S `test_single_memory_partial_reads`, E `probe_partial_stats` | PASS |
| Zones not written in the current frame read empty (aliasing probe) | DEC-33, ALG-STAT-03 | S `test_aliasing` | PASS (ASSUMPTION, owner query B20) |
| No false pipeline warnings (BPC dynamic detection no longer warns) | Review M3-R4, MAS §6.9 | V (warn sink), E (`SC_REPORT_WARNING` count) | PASS |
| FRAME_ID per started frame, shared by the three blocks, skipped by an aborted frame; CONTEXT_ID latched at SOF | ALG-STAT-01/04/09 | S `test_frame_id_abort_irq`, `test_context_latch`, V/E | PASS |
| Disabled block frozen | ALG-STAT-06 | S, V/E `stats_commit_sequence_4frames_64x48` | PASS |
| COMMON soft reset: results and done bits cleared, AEC zones untouched (min 4095) until the next enabled SOF, AWB ownership cleared, FRAME_IDs, counter and active set kept, pending commit kept and consumed at the next SOF | MAS Table 183, p248, owner Q4, ALG-STAT-07, M3-A8, U2-A1 | S `test_resets` | PASS |
| Post-reset tag sweep timing | ALG-STAT-08 | — | Not modelled (zero time) |

Mutation check (scratch copy, re-run 2026-10-01 after fixing the harness,
M3-R6): 41 single-rule mutations of the statistics. They cover accumulation
and tap rules, single-memory behaviour, publication and readout, resets, and
the false warning. 38 fail at least one test. The 3 survivors are
equivalent:
- the global count mask, which `sw_read` applies anyway;
- the OE mask, whose upper bits a 32-bit shift drops;
- "AWB limit read live", which, written at snapshot time, is the same as
  sampling at SOF.

## G-E2E — standalone end to end (M4)

| Req | Source | Test | Status |
|---|---|---|---|
| All 28 RAW frames: loader → RAM → IP → RAM → NV12, every frame bit-exact with the reference, statistics through every mux | DEC-03/04, DEC-34/35 | X (112 image runs, 196 frames) | PASS |
| Every pipeline block enabled on real frames (DG included) | DEC-34, review M4-R3 | X (`full`, `full_vga_709`) | PASS |
| Artefacts: manifest (input/profile/LUT checksums, geometry, strides, versions), NV12, preview, statistics, events, comparison | Plan §10 | X | PASS |
| Several frames per run (GTM curve, 2DNR variance, FRAME_IDs) through the rotation with recycle; the first frame (reset state) compared too | HAS §6.25.10, review M4-R2 | X (`full*`, `gtm_manual_qhd`: 2 frames, both compared), K | PASS |
| 4K (HAS maximum): every block, Resizer off and FHD (DEC-23 case) | HAS §2, DEC-37 | K (2 cases) | PASS |
| Boundary geometry: 4×4 minimum, crops on both axes, 3840×4, 4×2160 | ALG-DMS-03, HAS Table 6-17 | V, E (`edge_*`) | PASS |
| Host benchmark and simulated time | Plan M4 | Qualification report §6 | Recorded |

## G-CONT — continuous operation (M4)

| Req | Source | Test | Status |
|---|---|---|---|
| The 28 real frames back to back in one VP session (queue 4), each frame compared in order, temporal state carried across images | DEC-36, review M4-R1 | X (`sequence`, 4 presets × 28 frames) | PASS |
| 100+ frames, fixed seed, every completed frame compared; configuration changes between bursts; all five geometries and four Bayer orders | DEC-36 | C (53 bursts, 102 frames) | PASS |
| Rotation through 4 buffers, recycle, frame order, no loss or duplication outside the defined error behaviour | HAS §6.25.10, DEC-18/19 | C (outputs, counters, rotation after each burst) | PASS |
| Soft reset mid-frame (COMMON and DMA), AXI read/write errors, underrun, overflow, each followed by recovery | DEC-14, DEC-18/19, CSR-06 | C (27 bursts with events) | PASS |
| Aborted frames' SOF effects and failed outputs' EOF effects carried into later frames | ALG-STAT-09, DEC-19 | C (reference models both), mutation check | PASS |
| No deadlock | Plan §10 | C (every wait has a timeout) | PASS |
| Counter wrap: FRAME_ID, `IDMA/ODMA_FRAME_COUNT` | Plan §10 | C (fixture M4-A1) | PASS |

## M5 — handover

| Req | Source | Test | Status |
|---|---|---|---|
| Programming guide: bring-up, buffers, IRQ, configuration timing, statistics, errors | Plan M5 | `docs/ISP_PROGRAMMING_GUIDE.md` | Done |
| Reference driver (C99, bare-metal style) runs every vector and the error cases on the VP | Plan M5, DEC-02 | CTest `fx1_isp.integration.driver_tlm`; driver mutation check | PASS |
| CMake install and package; `add_subdirectory` use | Plan M5, F1 preparation | CTest `fx1_isp.package.consumer`; scratch parent project | PASS |
| Handover kit in the prefix: traceability, evidence, tools with the reference, sample profiles, 4K check runnable without Python or dataset | Plan P14, review M5-R8 | CTest `fx1_isp.package.consumer` (runs the installed 4K check and starts the installed tools) | PASS |
| Driver recovery with late interrupt handling (IDMA and ODMA) | Review M5-R5 | `driver_tlm` (late-handler cases) | PASS |
| Per-register traceability with a coverage gate (no unclassified register; every modelled register has a test or a vector use) | Plan G-CSR | CTest `fx1_isp.reference.traceability_up_to_date` | PASS |
| Per-block `STATUS.busy` | CSR, DEC-38 | E (mid-frame / after), C (stall vs idle), S `test_block_busy_and_warnings` | PASS |
| Registers without modelled hardware warn when enabled (OFMT padding, WDR, TNR) | DEC-39, M5-A1 | S `test_block_busy_and_warnings` | PASS |
| BLC profiles 0..3 with per-channel trims | HAS §6.7, M5-R3 | V, E, driver (`blc_profile*`) | PASS |
| Acceptance evidence with full SHA-256 kept in the repository; the regression compares with it | Review of M4 | `docs/evidence/`, CTest `fx1_isp.regression.raw --baseline` | PASS |

## CSR/MAS 07/10 — platform and handover (U6)

| Req | Source | Test | Status |
|---|---|---|---|
| AWB result ownership on the platform: report, acquire, release, clear; drops while software holds both banks, bank reuse; status, identity and sums equal the reference | MAS §6.22.4, plan U6 item 3 | `fx1_smoke_isp_mas0710` A (fixture `isp_fx_awb_script`) | PASS |
| Frame metadata through the driver on the platform; tags follow the frame when input and output indices differ | MAS Table 160, U5 | `fx1_smoke_isp_mas0710` A, `fx1_smoke_isp_recovery` A–E | PASS |
| DMA soft reset in the middle of a frame through the driver: frame dropped, no stale completion, stream continues from buffer 0, AWB lease and published statistics kept | MAS Table 183, U6-A1 | D `test_dma_reset_driver`; `fx1_smoke_isp_recovery` E, `fx1_smoke_isp_mas0710` B | PASS |
| DMA soft reset and driver state: AEC data trusted after an idle reset or one that drops only DONE frames, refused after a frame in progress; GTM hand-over completes after one frame taken after the reset, a pending curve still blocks before it, cold when nothing was taken (review U6-R1/R2) | MAS §9.1.1, §9.2, U6-A1 | D `test_dma_reset_stats`, `test_dma_reset_gtm` (normal and fast CSR) | PASS |
| DONE acknowledged even when the W1C meets the completion's core clock; a stale DONE on a buffer not in flight never makes an aborted frame's AEC data trusted (review U6-R3) | HAS Table 7-10 (hardware set wins), U6-A1 | D `test_done_ack_race`, `test_dma_reset_stats` (fast CSR) | PASS |
| Resizer on the platform: scaled from the first frame after start, mode switched between frames while streaming; NV12 equals the reference | MAS §6.20.4, §9.2 | `fx1_smoke_isp_mas0710` C | PASS |
| Concurrency (2 harts + ISP + SYS_DMA, C7/C12) still passes with the 07/10 model | Plan U6 item 3 | `fx1_smoke_shared_mem_concurrent`, `fx1_smoke_lrsc_masters` (CI) | PASS |
| Real RAW (OS04C10, HDR off): pixels unchanged against U0, statistics differences each attributed to a U2 rule | Plan U6 item 4 | `tools/run_regression.py` against the U2 baseline; workspace comparison against U0 | PASS |
| SDK package regenerated from the 07/10 model (profiles recomputed, goldens from the stand-alone model) | Plan U6 item 5 | `run_packaging_regression.sh`, `check_package.sh` (preview from an uncommitted tree) | PASS (preview) |

## Audit follow-up (2026-10-02)

| Req | Source | Test | Status |
|---|---|---|---|
| Coordinated DMA faults: IDMA + ODMA at once, late handler across several events, repeat after re-arm; ownership, exactly-once completion, order | Audit item 1 | `driver_tlm` fault campaign (48 runs, seed 20261002), `test_deferred_loss` | PASS |
| Reset while the target holds the engine inside `b_transport` | Audit item 1, M5-A5 | `dma_tlm` `test_reset_inside_blocking_target` (4 cases) | PASS |
| Statistics data refused after an abort or soft reset until a new publication | Audit item 2, M5-A4 | `driver_tlm` `test_stats_after_abort` | PASS |
| Driver API states, stale-recover idempotency, cancel, watchdog | Audit item 3, M5-A2/A3 | `driver_tlm` `test_driver_state_machine`, campaign | PASS |
| UB / container checks of the above | — | UBSan trap + `_GLIBCXX_ASSERTIONS`: driver, DMA, G-CONT tests | PASS |
| CPU integration ABI (memory map, IRQ, barriers, cache) | Audit item 4 | — | Deferred: CPU not chosen (DEC-08) |
| Image quality against tuned camera output | Audit item 5 | — | Deferred: needs owner tuning / paired data (DEC-05) |

## Later gates

| Gate | Milestone | Status |
|---|---|---|
| SPEC-04 buffer tags/timestamps | U5 | Done (MAS Table 160, see G-DMA) |
| CSR-14 Input Formatter crop window | — | GAP |
