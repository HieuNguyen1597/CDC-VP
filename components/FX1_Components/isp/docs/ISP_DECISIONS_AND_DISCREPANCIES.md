# FX1 ISP VP — Decisions and discrepancy log

Status values: **DECIDED** (user decision), **RESOLVED** (follows from DEC-09
or from unambiguous spec text), **ASSUMPTION** (model choice where the spec is
silent; observable, documented, may change), **GAP** (requirement that cannot
be implemented or tested with the current map), **OPEN** (needs a decision
before the named milestone).

CSR row numbers refer to worksheet `Registers` of the pinned XLSX
(see [ISP_SOURCE_MANIFEST.md](ISP_SOURCE_MANIFEST.md)).
Rows cited in §1–§8 are rows of the previous revision (`0980b61d…`); the CSR and
MAS revision of 2026-10-07 renumbers them. Entries marked **→ §9** are
superseded or changed by that revision; §9 gives the new contract and the
milestone of plan `ISP_CSR_MAS_UPDATE_PLAN_2026-10-09` that implements it.

## 1. User decisions

DEC-01..DEC-13 are recorded in the project plan (`plan/ISP_VP_PLAN.md` §5.1);
summary:

| ID | Decision |
|---|---|
| DEC-01 | Image algorithms follow the HAS; bit-exact only where the spec is complete; everything else is a documented assumption. |
| DEC-02 | SW rollout bare-metal/RTOS first, Linux V4L2 later. |
| DEC-03 | Acceptance requires RAW → pipeline → processed NV12 end to end. |
| DEC-04 | Real input: `Image Source`, BGGR, RAW10, 2688×1520. |
| DEC-05 | No owner golden/tuning available; the project builds its own reference. |
| DEC-06 | Event-based SystemC/TLM, configurable latency, not cycle accurate. |
| DEC-07 | New implementation in `components/FX1_Components/isp`; old `components/isp_tlm` is not a baseline. |
| DEC-08 | Standalone IP first; platform integration later. |
| DEC-09 | CSR wins over HAS where they differ. |
| DEC-10 | PNGs are real-camera ISP output; reference only, not golden. |
| DEC-11 | Bayer encoding 0=RGGB, 1=GRBG, 2=GBRG, 3=BGGR. **→ §9** |
| DEC-12 | Memory-to-memory topology only in this phase. |
| DEC-13 | Same-frame pairing / camera ISP identity of the PNGs unknown; not a blocker. |

Decisions taken during M0 (2026-09-30):

| ID | Topic | Decision |
|---|---|---|
| DEC-14 | Soft reset (CSR-01, CSR-02) | `COMMON_CTRL.soft_rst` and `DMA_CTRL.soft_reset` are identical: 32 core-clock periods, pixel pipeline + both DMA engines. **Preserved:** all RW configuration (including `IDMA_EN`/`ODMA_EN`), LUT/mesh contents, LSC profile validity, frame counters (`IDMA_FRAME_COUNT`, `ODMA_FRAME_COUNT`, `*_FRAME_ID`). **Cleared:** buffer ownership bits (`IDMA_BUF_VALID`, `ODMA_BUF_FREE`, `ODMA_BUF_DONE`), frame in flight, statistics results and done flags, `COMMON_IRQ_STATUS`, `DMA_IRQ_STAT`, `DMA_ERR`. **→ §9** |
| DEC-15 | Error aggregation (CSR-03) | `COMMON_STATUS.error` = OR of the current error flags `LSC_ERROR[5:0]`, `BPC_STATUS.cand_reject_ovf`, `EE_STATUS.error`, `DMA_ERR[4:0]` **including `IDMA_UNDERRUN`**. `COMMON_IRQ_STATUS.error_irq` is set whenever one of those flags goes from 0 to 1. |
| DEC-16 | `COMMON_CTRL.frame_start` (CSR-04) | Pending flag: SW writes 1, it reads 1 until hardware clears it at the next accepted SOF. It neither creates nor blocks frames; frames start from `IDMA_EN` + valid head buffer. **→ §9** |
| DEC-17 | `COMMON_STATUS.frame_done` (CSR-05) | Set on the same event as `frame_done_irq` (ODMA has the last write response of both planes); cleared at the next accepted SOF, by `i_rst_n` and by soft reset. |
| DEC-18 | ODMA overflow (CSR-19), decided 2026-09-30 | Lossless stall. When an output frame is waiting and the head output buffer is not free (ODMA enabled), set `DMA_ERR.ODMA_OVERFLOW` and `IRQ_ODMA_OVERFLOW` once per stall episode; the pipeline and IDMA are back-pressured and the frame completes intact once a buffer is freed. **→ §9** |
| DEC-19 | AXI error response (CSR-20) | Terminal for the frame, discarded end to end. IDMA error: `DMA_ERR.IDMA_AXI` + `IRQ_AXI_ERROR`, the buffer's `IDMA_BUF_VALID` bit is cleared (released), the rotation position is kept, so software re-arms the same index (underrun is reported meanwhile); no IDMA_DONE, no ODMA_DONE. ODMA error: `DMA_ERR.ODMA_AXI` + `IRQ_AXI_ERROR`, the buffer is not marked DONE, its `ODMA_BUF_FREE` bit is cleared, the rotation position is kept; the IDMA side is unaffected. |
| DEC-20 | ISP_EN ordering trap (HAS §9.1) | While `COMMON_CTRL.isp_en` = 0 the output geometry is 0×0, so arming the ODMA sets `DMA_ERR.ALIGN_OR_GEOMETRY` and the unit does not start. |
| DEC-21 | M2 datapath | Test-only stub with the real Input Formatter crop and output geometry: Y = cropped sample >> 4 (nearest-neighbour scaling), UV = 128. Replaced by the real pipeline in M3. |
| DEC-22 | Algorithm review policy (M3), decided 2026-09-30 | Where a HAS figure or block diagram contradicts the HAS numeric contract or equations, the equations win. Formulas are implemented literally even where the HAS makes a claim they refute (e.g. 2DNR Haar round trip, Resizer axis order). All other ALG gaps follow the review proposals in `plan/alg/ALG_*.md` as ASSUMPTIONs, and open points go to the owner query list. |
| DEC-23 | Resizer decimation stage count (ALG-RSZ-01) | Choose k by the HAS rule; if the decimated size is then smaller than the output on either axis, lower k until both axes are at least the output size (bilinear ratio ≤ 2). Identical to the HAS in every case where the HAS rule is sound. |
| DEC-24 | `CCM_CTRL.updated`, `RESIZER_CTRL.updated` (ALG-CCM-01, ALG-RSZ-05) | Commit gates: the CCM coefficient/offset set and the Resizer scale are copied to the active set only while `updated` = 1; hardware never clears the bit. Enables are sampled every SOF. **→ §9** |
| DEC-25 | EE parameter timing (ALG-EE-01/02) | Live, as the CSR states: EE scalars and LUTs take effect as written. Bit-exact comparison only for frames with no EE write in flight. **→ §9** |
| DEC-26 | Commit time for DEC-24/27 | With `updated` = 1 the commit happens immediately if the pipeline is idle (`COMMON_STATUS.busy` = 0), otherwise at the next accepted SOF. `RESIZER_OUT_W/H` derive from the committed scale. **→ §9** |
| DEC-27 | `CNF_CTRL.updated` | Commit gate as DEC-24 (CSR wins over HAS §6.19 "not implemented"). **→ §9** |
| DEC-28 | BPC dynamic detection (ALG-BPC-01/02) | Not modelled: detection is always static (bit-exact); with `dynamic_det_en` = 1 the model warns once and the candidate/defective counters read 0. GAP for the owner. **→ §9** |
| DEC-29 | Statistics buffering (ALG-STAT-02/03) | As the HAS text: one result memory per block; zone and histogram contents of frame N are readable until the SOF of frame N+1 overwrites them. FRAME_ID is assigned when a frame enters the pipeline and travels with it (ALG-STAT-01 proposal). Refined by DEC-30. |

Decisions from the M3 review (2026-10-01):

| ID | Topic | Decision |
|---|---|---|
| DEC-30 | Statistics memory during a frame (review M3-R5) | The AEC zone + histogram memory and the AWB zone memory are updated in place, row by row, as the frame streams. From the SOF of an enabled frame, a zone already written in that frame reads its partial sums; a zone not yet written reads the empty value (0; min 4095, max 0); the histogram reads the partial counts. Global results (AEC/AWB), AF scores, FRAME_ID and RESULT_CONTEXT_ID keep the previous publication until EOF. This replaces the ALG-STAT-02 (a) proposal, under which the view read empty until EOF and hid the race a driver must see. **→ §9** |
| DEC-31 | LSC mesh ceiling (ALG-LSC-11, owner query B17) | 2..32 nodes per axis, from top-level `PARA_LSC_MESH_MAX` (HAS Table 5-1), until the owner confirms. **→ §9** |
| DEC-32 | Pending `AEC_CTRL.commit` at soft reset (B18) | Dropped, per the DEC-14 W1S rule; the AEC active configuration is kept (was M3-A3). **→ §9** |
| DEC-33 | Zones not written in frame N (ALG-STAT-03, B20) | Read empty, even where a 1-bit RTL tag could match frame N−2. ASSUMPTION pending RTL/owner. This does not change DEC-30: zones written in the current frame read their partial sums. |

M4 scope decisions (2026-10-01), detail in [ISP_M4_QUALIFICATION.md](ISP_M4_QUALIFICATION.md) §1:

| ID | Topic | Decision |
|---|---|---|
| DEC-34 | Real-RAW profiles | Four test presets (`basic`, `full`, `full_vga_709`, `gtm_manual_qhd`) from `tools/make_profile.py`; formulas, not calibrations. `full` enables every pipeline block, DG included. |
| DEC-35 | Real-RAW regression | `tools/run_regression.py` with the plan §10 artefacts outside the tree; optional CTest with `-DFX1_ISP_RAW_INVENTORY`. |
| DEC-36 | G-CONT | (a) The 28 real frames back to back in one VP session, for each preset. (b) 100+ synthetic frames from a fixed seed, compared with the reference, with soft resets, AXI errors, underrun and overflow injected. |
| DEC-37 | 4K | Deterministic pattern in C++ and Python; only the SHA-256 and the statistics readout are committed. |

M5 decisions (2026-10-01):

| ID | Topic | Decision |
|---|---|---|
| DEC-38 | Per-block `STATUS.busy` of Demosaic, CCM, Gamma, GTM, 2DNR, EE, CNF and Resizer (CSR: "the block currently holds pixels", used to tell an idle pipeline from a stalled one) | Frame granularity: set at the accepted SOF; cleared when the pipeline has emitted the frame's last row, when the frame is abandoned, and by either reset. The bits stay 1 during a stall (`isp_en` cleared mid-frame, ODMA overflow). A bypassed block still carries the stream. ASSUMPTION: no per-block row-level timing. |
| DEC-39 | OFMT stride padding (`OFMT_CTRL.stride_en`, owner query B14, ALG-OFMT-01) | Not modelled. With `stride_en` = 1 the output is unchanged (in the memory-to-memory topology the ODMA applies the strides), and the pipeline warns once per frame. |

## 2. Specification discrepancies from the plan (SPEC-xx)

| ID | Summary | Status |
|---|---|---|
| SPEC-01 | Bayer enum listed twice in CSR row 29 | DECIDED (DEC-11); generator override asserts the stale list |
| SPEC-02 | DMA soft reset scope/duration: HAS 15 cycles DMA only, CSR 32 cycles pipeline+DMA | RESOLVED by DEC-09, extended to COMMON soft reset by DEC-14 |
| SPEC-03 | CSC: HAS build-time constants vs `CSC_CTRL.std` | RESOLVED: implement `std`, 0=BT.601 (reset), 1=BT.709, sampled at SOF |
| SPEC-04 | HAS §6.25.10.3 buffer tags/timestamps, `FRAME_COUNTER` have no CSR | **GAP**: not implemented, no offsets invented; internal trace only |
| SPEC-05 | HAS `IRQ_CFG_ERROR` not in CSR | RESOLVED: `DMA_IRQ_*` bit 0 tied to zero; ALIGN reported via DEC-15 **→ §9** |
| SPEC-06 | HAS `DMA_WIDTH/HEIGHT` (3840×2160) vs COMMON geometry (1920×1080) | RESOLVED: COMMON geometry only |
| SPEC-07 | `ODMA_BUF_FREE` cleared at buffer start (HAS) vs when full (CSR) | RESOLVED: SW-visible FREE clears when full; internal in-use state prevents reuse (M2) **→ §9** |
| SPEC-08 | 2–8 buffers (HAS) vs 4 (CSR) | RESOLVED: 4 buffers, bits [7:4] unused |
| SPEC-09 | RAW bit depth | RESOLVED: no depth register; fixture shifts source `>> 4` (plan §4) |
| SPEC-10 | `IDMA_STRIDE` reset 7680 with a "1920 pixels" comment | RESOLVED: numeric 0x1E00 kept (it matches HAS 3840×2 bytes) |

## 3. New findings from M0 (CSR-xx)

| ID | Evidence | Status / resolution | Affects |
|---|---|---|---|
| CSR-01 | Row 10: COMMON soft reset "behaves like DMA_CTRL.soft_reset"; duration only given for DMA (row 451, 32 cycles). HAS Table 9-1 differs per source. | DECIDED (DEC-14) | M1, M2 |
| CSR-02 | HAS Table 9-1 does not list counters or `DMA_ERR` | DECIDED (DEC-14) | M1, M2 |
| CSR-03 | Rows 15/18: error sources are "any block" | DECIDED (DEC-15) | M1 |
| CSR-04 | Row 11: "software-triggered mode" has no mode register; HAS §9.1 IDMA self-starts | DECIDED (DEC-16) | M1, M2 |
| CSR-05 | Row 14: `frame_done` clear rule not given | DECIDED (DEC-17) | M1, M2 |
| CSR-06 | Row 457: `IDMA_UNDERRUN` is "level; set while it persists" in a W1C field | RESOLVED by HAS §6.25.8.3: level-held, a W1C write does not clear it while the condition persists. ASSUMPTION: `DMA_IRQ_STAT.IDMA_UNDERRUN` and `error_irq` are set on the rising edge of the condition only (no re-trigger while it persists). | M1 mechanism, M2 source |
| CSR-07 | Rows 403–404: `AF_CTRL.search_start/abort` are W1S with no hardware | ASSUMPTION: behave as a W1S flop that nothing clears — reads 1 after a write until `i_rst_n`. Soft reset clears it (it is command state, DEC-14). | M1 |
| CSR-08 | Rows 163, 275, 286: `CCM_CTRL.updated`, `CNF_CTRL.updated`, `RESIZER_CTRL.updated` are RW; HAS §6.19 says CNF's is not implemented; who clears CCM/Resizer's is not stated | M1: plain RW storage. Datapath meaning decided at M3 (proposal: no hardware clear; CCM/Resizer take config at SOF as every block does). | M3 |
| CSR-09 | Rows 288–291: `RESIZER_OUT_W/H` derived by hardware; latch point not stated; HAS says output geometry is "only driven while ISP_EN is set" but the reset values (1920×1080) are readable with ISP_EN=0 | ASSUMPTION: register readback is derived live from `COMMON_FRAME_*`, `COMMON_BAYER` and `RESIZER_CTRL.{en,scale}`, not gated by ISP_EN. The ISP_EN gating applies to what the ODMA captures (M2). | M1, M2 |
| CSR-10 | Row 25 says width "must be even after any phase crop"; HAS Table 6-17 rounds down to even | RESOLVED: HAS rule `W_out = floor((W − S_H)/2)·2`, same for height | M1 (OUT_W), M3 |
| CSR-11 | HAS §6.25.8.2 sets `DMA_STAT.IDMA_FRAME_DONE`; CSR `DMA_STAT` has only busy bits | RESOLVED (DEC-09): no such bit | M2 |
| CSR-12 | HAS names `DMA_ERR.ALIGN/AXI_RD/AXI_WR`; CSR names `ALIGN_OR_GEOMETRY/IDMA_AXI/ODMA_AXI` | RESOLVED: CSR names | — |
| CSR-13 | HAS `MAX_BURST_BEATS` 1..256 vs CSR `max_burst_m1` | RESOLVED: beats = `max_burst_m1 + 1` | M2 |
| CSR-14 | HAS §6.20.2 tells SW to program an "Input Formatter crop window"; no such CSR | **GAP**: only the Bayer phase crop exists | M3 |
| CSR-15 | HAS Table 6-4 lists `AF_CTRL.BYPASS` as a control; CSR row 401 says no hardware | RESOLVED (DEC-09): no hardware | M3 |
| CSR-16 | HAS §6.5.2: LUT memories undefined after reset; CSR: Gamma LUT "powers up as zeros" (row 191), EE tables "reset to" 0x8000 (row 267) | ASSUMPTION: on `i_rst_n` the model fills Gamma = 0, EE = 0x8000, GTM = 0 (GTM unstated). Deterministic, matches CSR text; SW must still load tables before enabling (HAS §9.1). Soft reset preserves all tables. | M1 **→ §9** |
| CSR-17 | HAS §6.21.6.2: AEC shadow set includes "image height"; no AEC height register | RESOLVED in M3 (ALG-AEC-05, ALG-STAT-10): the frame's Input Formatter output geometry, taken at every accepted SOF | M3 |
| CSR-18 | Rows 94–98, 208: LSC load sequencer and GTM LUT write port depend on block state | Implemented in M3 (see [ISP_CSR_CONTRACT.md](ISP_CSR_CONTRACT.md) §6) | M3 |
| CSR-19 | HAS §6.25.9.3 ODMA overflow back-pressures losslessly vs Table 6-100 "frame is corrupt"; CSR row 457 "a frame completed with no free output buffer" | DECIDED (DEC-18) | M2 |
| CSR-20 | HAS §7.1.8 "returns to idle, releasing the buffer" vs §6.25.11 "without advancing the rotation … software must re-arm" | DECIDED (DEC-19) | M2 |
| CSR-21 | Rows 193–196, 264–267: LUT address auto-increment; wrap and partial-strobe behaviour not stated | ASSUMPTION: an accepted write to the data port (any strobe set) writes the merged data register value to the entry at the current address, then the address increments modulo its field width (Gamma 256, EE 64; EE contrast tables use `addr & 31`). A write with no strobes changes nothing and does not increment. | M1 |
| CSR-22 | Row 453: `DMA_STAT` 0 on a wedged engine | Informational; model reports busy only while a transfer is outstanding | M2 |
| CSR-23 | HAS Table 7-10 "address bits 1:0 ignored" vs sub-word TLM accesses | ASSUMPTION (adapter contract): a 4-byte TLM access addresses the word `addr & ~3` with all lanes (bits [1:0] ignored); a 1–3 byte access uses byte lanes `addr[1:0]..` of that word and must not cross it (else `TLM_BURST_ERROR_RESPONSE`, since it is not one AXI4-Lite beat). See [ISP_CSR_CONTRACT.md](ISP_CSR_CONTRACT.md). | M1 |

## 4. M2 model assumptions (DMA and frame lifecycle)

| ID | Assumption | Basis |
|---|---|---|
| M2-A1 | The IDMA and ODMA rotation positions return to buffer 0 on `i_rst_n` and on soft reset (sequencing state). | HAS Table 6-12 resets every state machine and counter; the HAS is silent on soft reset |
| M2-A2 | Clearing `IDMA_EN` / `ODMA_EN` takes effect at a frame boundary: the frame in progress completes, no new frame starts. The IDMA checks its enable at every arm; the ODMA checks its enable at arm **and again before accepting a frame's SOF**, so an ODMA that is armed and then disabled keeps its captured buffer and waits (review M2-R1). | HAS §9.4.2 "stop the input DMA at a frame boundary" |
| M2-A3 | Address bits above `PARA_AXI_ADDR_WIDTH` (40) are dropped; alignment is checked on the 40-bit address. | The ports have 40 address wires (HAS Table 5-1) |
| M2-A4 | If the frame reaching the ODMA does not have the geometry captured at arm, `ALIGN_OR_GEOMETRY` is set, the frame is discarded and the ODMA re-arms. | HAS §9.4.2: geometry changes only while idle; behaviour otherwise unspecified |
| M2-A5 | Clearing `ISP_EN` mid-frame stalls the pipeline at the next line; setting it again resumes. Soft reset is the recovery path. | CSR row 9 "stops all processing" |
| M2-A6 | With `ODMA_EN` = 0 an output frame waits without raising `ODMA_OVERFLOW`. | Overflow is about buffer availability, not a disabled engine |
| M2-A7 | `DMA_STAT.idma_busy` is 1 from arm to retire; `odma_busy` from the first output line to completion or abort. | CSR row 454–455 "transfer outstanding" |
| M2-A8 | After an `ALIGN_OR_GEOMETRY` refusal the unit re-checks on every register write; the sticky bit is set again only after software cleared it. | HAS Table 6-100 "the unit does not start" |
| M2-A9 | On an IDMA error after line 0, lines already forwarded may have been written into the armed output buffer; that buffer is not DONE and stays FREE, and the next frame overwrites it. | DEC-19; no partial-frame recovery (HAS §7.1.8) |
| M2-A10 | Transactions are issued one burst at a time (no outstanding-transaction overlap); the two ODMA channels are serialised per line (Y line, then UV line) instead of round-robin per burst. Memory latency is the target's annotated delay. | DEC-06 (LT model); observable ordering within a line is not specified |
| M2-A11 | Pipeline pacing: one input line takes `(W + pipeline_line_overhead_cycles)` core cycles. The IDMA retires a frame when the pipeline has accepted its last line. | HAS §6.4 (1 ppc), §6.25.8.2 step 5 |
| M2-A12 | HAS Table 6-100 "the frame is corrupt" for output overflow is superseded by DEC-18. | DEC-18 |

Review findings fixed in M2:

| ID | Finding | Fix | Regression |
|---|---|---|---|
| M2-R1 | An ODMA armed and then disabled still wrote the next frame | `ODMA_EN` re-checked before accepting SOF | `test_odma_disabled_after_arm` |
| M2-R2 | A reset did not wake an engine sleeping on a timed wait, so a re-armed frame waited out the old line latency | Every engine wait is also woken by the reset event, and the epoch is checked before and after it | `test_reset_wakes_line_wait` (second DUT, 2 ms per line) |

## 5. M3 algorithm review (ALG-xx)

The per-block arithmetic review (P02) was done before each M3 batch. The
review sheets are kept with the plan, outside this repository, because they
quote HAS equations: `plan/alg/ALG_A_ifmt_blc_lsc_bpc.md`,
`ALG_B_wb_dg_demosaic_ccm.md`, `ALG_C_gamma_csc_gtm.md`, `ALG_D_2dnr_ee.md`,
`ALG_E_cnf_resizer_ofmt.md` and `ALG_F_aec_awb_af.md`. Each sheet lists the
integer algorithm, the CSR fields used and when they are sampled, the border
policy, and the gaps as `ALG-<block>-nn`. Under DEC-22 every PROPOSAL in those
sheets is implemented as an ASSUMPTION unless a DEC entry above overrides it.
The code cites the ALG identifier at each such choice. Open points for the IP
owner are in `plan/alg/OWNER_QUERIES.md`.

Model-level choices made in M3 that are not on the review sheets:

| ID | Assumption | Basis |
|---|---|---|
| M3-A1 | The model is row streaming: each window block emits row y once row y+r has arrived, as a line buffer would, and the Python reference is frame based. The two are written independently, and agreement is checked bit for bit. | DEC-05, DEC-06 |
| M3-A2 | `EE_STATUS.error` (radial gains out of range) is a level condition. It is re-evaluated on every EE write and right after a soft reset, so it reasserts at once if the condition still holds. | ALG-EE, DEC-14 |
| M3-A3 | Superseded by DEC-32 (same behaviour, now decided). | DEC-32 |
| M3-A4 | `stats_ready_irq` is set on every publication, even when the block's done bit is still set from an unread earlier frame. It is an event, not the rising edge of the done bit. | HAS Table 9-2, §6.21.6.5, ALG-STAT-11 |
| M3-A5 | The 2DNR noise-variance shadow is a measured value, cleared by soft reset like the other results. | ALG-2DNR-09, DEC-14 |
| M3-A6 | Superseded by DEC-31 (2..32 nodes per axis; the LSC block parameters give 128). | DEC-31 |
| M3-A7 | An aborted frame (IDMA AXI error, an SOF in the middle of a frame, unsupported geometry) publishes no statistics and clears the AEC/AF busy bits. It still consumed a FRAME_ID. Its rows remain in the zone/histogram memory (DEC-30): software sees partial data with the previous FRAME_ID and must rely on the done bit. | ALG-STAT-09 |
| M3-A8 | Before the first enabled SOF after `i_rst_n` or a soft reset, every zone and histogram field reads the CSR reset value 0, including `green_min`. From that SOF on, DEC-30 applies. The post-reset tag sweep takes no time (ALG-STAT-08). This refines ALG-AEC-04 (a), which said "until the first publication". | ALG-AEC-04, ALG-STAT-08 |
| M3-A9 | The demo tool `fx1_isp_run_raw` queues one buffer and leaves `IDMA_EN` set, so `DMA_ERR.IDMA_UNDERRUN` (0x2) is expected at the end of the run. | HAS §6.25.8.3, CSR-06 |

Review findings fixed in M3:

| ID | Finding | Fix | Regression |
|---|---|---|---|
| M3-R1 | `stats_ready_irq` was derived from the 0→1 edge of each done bit, so a new publication raised no interrupt while software had not cleared the previous done bit | Explicit set per publication (M3-A4) | `test_stats` `test_frame_id_abort_irq` |
| M3-R2 | Mutation testing: the pipeline vectors did not catch the EE gain order, CNF inclusive thresholds or the 2DNR lower median | Block-level vectors (`tests/data/blocks`) | `fx1_isp.unit.block_vectors` |
| M3-R3 | Mutation testing: no statistics vector had WB or GTM enabled or a grid smaller than the frame, so the tap points and the last-zone clamp were untested | Vectors strengthened | `stats_scene_bggr_2frames_134x98`, `stats_flat_channels_64x48` |
| M3-R4 | Review: `snapshot_config()` still listed AEC/AWB/AF as "not implemented", so every enabled frame logged a false "passed through" warning | Mechanism removed | Pipeline vectors (warn sink) and E2E (`SC_REPORT_WARNING` count): only the DEC-28 warning, once per frame of `bpc_dynamic_requested_64x32` |
| M3-R5 | Review: zones and histogram read empty from SOF to EOF, so a read 5 lines into a frame got 0 instead of the partial sums | DEC-30: the pipeline writes the control unit's single memory row by row | `test_stats` `test_single_memory_partial_reads` (every row of a frame), E2E `probe_partial_stats` (over the CSR socket, mid-frame) |
| M3-R6 | Mutation harness: restoring a file with `rsync -a` kept its old mtime, so ninja did not rebuild it and one mutant could leak into the next run. A test value also hid a missing UE mask | Harness copies by checksum with a fresh mtime; batch re-run in full; test value changed | Mutation log 2026-10-01: 38 of 41 killed, 3 equivalent |

## 6. M4 model and test-bench choices

| ID | Choice | Basis |
|---|---|---|
| M4-A1 | `fx1_isp_tlm::debug_set_frame_counter()` (testbench backdoor, like the other `debug_*` methods) sets the statistics frame counter, so the FRAME_ID wrap at 2^32 can be tested without 2^32 frames. | Plan G-CONT "counter wrap checked with a suitable fixture" |
| M4-A2 | `fx1_isp_run_raw` is a driver-like harness. One profile is applied once. Frames come from one input, an input list or the synthetic pattern, with up to 4 in flight through the 4-buffer rotation. The first buffers are queued before the DMA is enabled. Each output is read out after its DONE and recycled, and the interrupt status is read and acknowledged per frame. `--per-frame` writes every frame's NV12; `--check-stats` and `--expect-sha256` make it a self-checking test. | DEC-35, DEC-36, DEC-37 |
| M4-A3 | In G-CONT, configuration changes only between bursts, with the pipeline idle (HAS §9.4.4). Inside a burst, frames queue back to back. Events are injected in the first frame of a burst, so the frames they affect are deterministic. | DEC-36 |

Review findings fixed in M4 (detail in [ISP_M4_QUALIFICATION.md](ISP_M4_QUALIFICATION.md) §8):
- M4-R1: no real-frame sequence in one VP session.
- M4-R2: only the last frame of two-frame runs was compared.
- M4-R3: `full` did not enable DG.
- M4-R4: the synthetic sequence lost the BGGR geometry when regenerated.
- M4-R5: start-up underrun in the driver harness.

## 7. M5 findings and choices

| ID | Item | Resolution |
|---|---|---|
| M5-A1 | `D_WDR_*` and `TNR_3D_*` registers: HAS §3.1 lists WDR and 3DNR as unsupported | Storage only; enabling either warns once per frame (like DEC-28 and DEC-39) |
| M5-R1 | The per-register traceability found that the 8 block `busy` bits were never set (CSR-defined status) | DEC-38; tests in E2E (mid-frame), G-CONT (set during an overflow stall, clear when idle) and `test_stats` |
| M5-R2 | The same review found that OFMT `stride_en` was ignored and nowhere documented | DEC-39 |
| M5-R3 | BLC profiles 2 and 3 and the profile-0 per-channel trims had never been written by a test; the model selects profiles by offset arithmetic | 3 vectors with decoy values in the other profiles (`blc_profile*`), bit-exact |
| M5-R4 | Mutation testing of the reference driver: first version of its test missed wrong-buffer recovery, a missing DONE acknowledge, the queue limit and the start-up order | Distinct inputs per frame, a late interrupt handler, 4 frames in flight, explicit checks; 10 of 15 mutants killed, 5 equivalent (programming guide §9) |
| M5-R5 | Review: after an ODMA AXI error, the driver freed `out_done`, the next buffer to collect. With a late interrupt handler that buffer is a completed frame not yet collected, so the failed buffer was never freed (stream stuck) and the completed one could be overwritten | The failed buffer is the first in-flight one, in rotation order, whose DONE bit is clear. Test: ODMA error with the handler run only after the previous frame is DONE, then 4 frames in flight; checks the order, `in_flight` and the rotation. The old code deadlocks this test |
| M5-R6 | Found by the new install test: the installed `fx1_isp_run_raw` had no RPATH to `libsystemc` (only the build tree had one) | `INSTALL_RPATH` from `SystemC::systemc`; the package test runs the installed tool |
| M5-R7 | Review: `run_regression.py --only <frame> --baseline` failed on the subset's own "sequence of N" runs, which the 28-frame baseline cannot contain | A subset compares the runs the baseline has and reports the others as not compared; a full run still requires every run to match |
| M5-R8 | Review: the install prefix lacked the traceability report, the evidence, the scripts and sample configurations listed for the handover (plan P14) | Installed under `share/doc/fx1_isp` and `share/fx1_isp` (tools, reference, schema, samples, 4K check). README "Handover package" lists what needs the source checkout |

## 8. Follow-up of the logic audit of 2026-10-02

The audit (report `VP_BK/ISP/report/ISP_VP_LOGIC_AUDIT_2026-10-02.md`) fixed A1–A4 and listed five further
improvements. Items 1–3 are done below. Item 4 (CPU integration ABI) waits for the CPU choice (DEC-08).
Item 5 (image quality) waits for owner tuning and paired data (DEC-05).

| ID | Item | Resolution |
|---|---|---|
| M5-R9 | Coordinated fault campaign (item 1) found a driver defect. When the ODMA failed on a frame while the IDMA was still reading its input, the driver declared the frame lost at once. If the IDMA then also failed on that frame, the hardware waited for a re-arm of that input. The driver re-armed another input (IDMA stuck) or the frame completed after the caller resubmitted it (duplicate). | A failed-output frame stays in flight, marked, until the IDMA has retired its input without an error (then lost, reported by `fx1_isp_next_lost`) or the IDMA error names it (then retried). IDMA_DONE is enabled to trigger the decision (programming guide §8.1). |
| M5-A2 | `fx1_isp_recover` acts only on error bits still set in `DMA_ERR`. A repeated call with the same snapshot is a no-op. | Item 3; campaign checks it after every recovery. |
| M5-A3 | Driver state machine UNINIT/CONFIG/STREAMING with `FX1_ISP_ESTATE`; `fx1_isp_stop` cancels a stream; `fx1_isp_get_progress` supports a caller watchdog. The timeout budget stays the caller's choice (no silicon figure). | Item 3; guide §8.2. |
| M5-A4 | Zone/histogram data after a software-visible abort (IDMA error), a soft reset or `stop` is refused (`FX1_ISP_ENODATA`) until `AEC_FRAME_ID` moves past the FRAME_ID recorded at the latest such event. It is not refused with FRAME_ID 0. Internal aborts of the model (SOF mid-frame, block geometry rejection) are not visible to software and not covered. No CSR bit was added. | Item 2; guide §5. |
| M5-A5 | Reset while the engine thread is suspended inside the target's `b_transport`: the transfer returns into an old epoch and is dropped; no access starts after the reset, no completion, retry correct. | Item 1; `test_reset_inside_blocking_target` (IDMA/ODMA × soft/external), test memory `blocking` mode. Mutation: dropping the epoch check after `b_transport` fails 10 checks. |

## 9. CSR/MAS revision of 2026-10-07 (U0, plan ISP_CSR_MAS_UPDATE_PLAN_2026-10-09)

The CSR revision `bda077b6…` (254 registers, 336 fields) and the MAS of the
same date replace the pinned map `0980b61d…`. Under the user decision of
2026-10-09, an entry is superseded where the new CSR or MAS is explicit; where
it is silent or contradictory, the entry stays and the point is listed in
§9.3 for the owner. MAS pages are PDF pages of the 336-page MAS. The ABI
delta and the generator changes are in [ISP_CSR_REVISION_0710.md](ISP_CSR_REVISION_0710.md).

### 9.1 Superseded or changed entries

| Entry | Previous VP contract | 07/10 contract (source) | Status | Milestone |
|---|---|---|---|---|
| DEC-11 | Bayer encoding adopted over the CSR enum list | The CSR enum list now has the same encoding | RESOLVED in the CSR; the generator asserts it | U0 (done) |
| SPEC-05 | `DMA_IRQ_*` bit 0 tied to zero | Bit 0 is `CFG_ERROR`: any illegal start configuration sets it with `DMA_ERR.ALIGN`, and the unit does not start (MAS Table 161–162, p294–295) | SUPERSEDED | U0 (ABI), U1 (behaviour, done) |
| DEC-14 | Both soft resets identical, 32 core clocks | 15-core-clock W1SC pulse, restart on rewrite. `COMMON_CTRL.soft_rst`: pipeline + both DMA engines, clears result ownership/status. `DMA_CTRL.soft_reset`: DMA and the affected frame only, keeps CSR configuration, statistics and completed/leased AWB results (MAS p33, Table 183 p316–317, p248) | SUPERSEDED | U1 (done) |
| DEC-32 | Pending `AEC_CTRL.commit` dropped by soft reset | Kept across soft reset, consumed at the next accepted ingress SOF; hard reset clears it (owner answer Q4 2026-10-09; MAS §6.21.4.2) | SUPERSEDED | U1 (done) |
| DEC-24, DEC-26, DEC-27 | `CCM/CNF/RESIZER_CTRL.updated` commit gates, immediate commit when idle | `updated` is a compatibility field with no effect. CCM and CNF are captured at the accepted SOF; the Resizer publishes a derived snapshot (E0–E35) (owner Q1; MAS §6.20.4) | SUPERSEDED | U1 (CCM, CNF: done), U4 (Resizer: done) |
| CSR-16 | Gamma = 0, EE = 0x8000, GTM = 0 at `i_rst_n` | RAM contents are undefined after reset (MAS §6.5.2 p33, p317, p334). The VP fills a fixed deterministic poison pattern (user decision 2026-10-09) | SUPERSEDED | U1 (done) |
| DEC-31 | LSC mesh 2..32 nodes per axis | 2..128 nodes per axis, not above the active width/height. A failed validate or an abort leaves the destination invalid (owner Q5; MAS §6.8.3–6.8.4 p55–61) | SUPERSEDED | U1 (done) |
| DEC-28 | BPC dynamic detection: warning, counters read 0 | Spatial BPC only; `dynamic_det_en`, `pixel_age` and `temporal_var` have no effect. The counters clear at frame start, count during the frame and saturate at 16383 (MAS §6.9 p67–78) | SUPERSEDED | U2 (done) |
| DEC-30 (AWB part) | AWB zone memory updated in place | Two result banks with acquire/release; reads without a lease return 0; a frame on an illegal grid is not published (MAS §6.22.3.7, §6.22.4 p246–248). The AEC part of DEC-30 stays | SUPERSEDED for AWB | U2 (done) |
| M2-A4 | A frame whose geometry differs from the one captured at ODMA arm is discarded with `ALIGN` | The ODMA takes each frame's line length and count from the frame, without re-arm; strides are checked at arm (MAS §9.2) | SUPERSEDED | U4 (done) |
| AEC untouched zones after reset | `GREEN_MIN_MAX` read the CSR reset value 0 until the first enabled SOF | Reset clears the written flags, so an untouched zone reads min 4095, max 0 (MAS §6.21.4.3, p237) | SUPERSEDED | U2 (done) |
| ALG-AF-01/02 | AF `EN` read live per row; partial frame publishes with `SCORE_VALID` = 0 | `EN` is sampled once per frame as it enters the pipeline (MAS p260, p264) | SUPERSEDED | U2 (done) |
| DEC-25 | EE scalars and LUTs live | Three resident profiles. `PROFILE_SEL` and the scalars are captured at the accepted window-centre SOF; `PROFILE_WR_SEL` selects the programmed profile; LUTs stay live, so writes to an in-use profile can tear (MAS §6.18.4.1 p188–189, §9.2 p318). User decision: model tearing per row | SUPERSEDED | U3 (done) |
| GTM LUT write gate (CU `!EN \|\| MANUAL`) | Writes accepted while disabled or MANUAL | Writes update the table only while `EN` = 0; `MANUAL` only holds automatic construction (MAS p146) | SUPERSEDED | U3 (done) |
| SPEC-07 | SW-visible `ODMA_BUF_FREE` clears when the buffer is full | Cleared when the output frame starts (MAS p290, Table 159 p292) | SUPERSEDED | U5 (done) |
| DEC-18 (status part) | `ODMA_OVERFLOW` set once per stall episode | Held as a level while the stall lasts; the lossless stall itself is unchanged (MAS §6.25.3.9 p291) | SUPERSEDED (status) | U5 (done) |
| SPEC-04 | Buffer tags, timestamps and `FRAME_COUNTER` have no CSR: not implemented | `IDMA_BUF_TAG/TS0..3` (RW), `ODMA_BUF_TAG/TS/SOF_TS/EOF_TS0..3` (RO), `FRAME_COUNTER` (RO) at `0x3100`–`0x3160`; captured at frame start, published at completion (MAS §6.25.4.1.0.3, Table 160 p293–294) | RESOLVED | U5 (done) |

### 9.2 Kept, because the 07/10 contract agrees

CCM arithmetic, Gamma, WB, CNF code 511, 2DNR variance, LSC strength and spacing,
Input Formatter dimensions, DMA burst splitting and alignment, AXI error handling,
AEC context/FRAME_ID capture and commit transfer: checked against the MAS in U0,
no change.

### 9.3 Open points for the owner (batched for U6)

| ID | Point |
|---|---|
| U0-Q1 | `COMMON_CTRL.frame_start`: the CSR says it is needed only to release the first frame, while MAS §6.25.3.6 starts the IDMA on `IDMA_EN` and a valid head buffer. Does the first frame wait for it, and does it read 0 or 1 until SOF? DEC-16 stays until answered. |
| U0-Q2 | Crop Y must be even (CSR only): a software constraint, or a hardware error? |
| U0-Q3 | AF `SCORE_VALID`: meaning beyond "identifies published scores" (the MAS body does not define it). |
| U0-Q4 | BPC counters while `BPC_EN` = 0: counted or not? |
| U0-Q5 | Effect of a soft reset on the AEC zone "untouched" state (min = 4095). |
| U0-Q6 | LSC: does `COEF_DATA` while idle, or a destination ≥ 3, set `LOAD_PROTOCOL`? (the model sets it) |
| (open) | Three-output architecture; LSC `LOAD_ADDR` after coefficient 65536 (follow-up of 2026-10-09). |

### 9.4 U1 implementation notes (2026-10-09)

Assumptions taken where the MAS is silent. Each one is checked by a test and
goes to the owner with §9.3.

| ID | Assumption | Test |
|---|---|---|
| U1-A1 | `DMA_CTRL.soft_reset` keeps `COMMON_IRQ_STATUS`. Table 183 limits its scope to the DMA block (offsets from `0x3000`) and the busy flag. | U `test_soft_reset` (DMA scope) |
| U1-A2 | A geometry mismatch found when a frame arrives (the unit has already started) sets `DMA_ERR.ALIGN` only. `IRQ_CFG_ERROR` is reserved for a refused start (MAS Table 161: illegal start configuration). | D `test_alignment_rejection`, `test_isp_en_trap` |
| U1-A3 | Poison pattern `((i·0x9E37) ⊕ 0xA5A5) & mask`, with index `64·t + i` for EE and `65·b + i` for the GTM banks. Model and reference are identical. Software must load every LUT it enables. | U `test_external_reset`, EE port test |

Test data that changed with U1. Each change was attributed to one rule by
undoing the rules one at a time (workspace `output/u1_attribution/`):

- `gbrg_ccm_gate` vector: the CCM gate is gone, so the matrix applies at SOF.
- `edge_*` vectors: the generator now clamps the mesh to the image (DEC-31),
  so LSC applies on these small images.
- G-CONT `cont_100`: one frame differs, because of the DMA-only reset scope.
- The generators load unity EE tables before enabling EE; otherwise poison
  RAM would reach the output. With this, `full_chain_*` and the 4K expected
  SHA-256 are unchanged.
- Full-preset profiles load LSC into profile 1 and select it, since a begin
  into the active profile is rejected.

Corrections after the U1 review of 2026-10-09 (`report/ISP_CSR_MAS_U0U1_REVIEW_2026-10-09.md`):

| ID | Defect | Fix | Test |
|---|---|---|---|
| U1-R1 | A DMA-only soft reset abandoned the frame in flight but left the busy flags of the 8 pixel blocks and of AEC/AF set, so firmware waiting for idle could time out | The DMA scope also ends the in-flight frame state, like any other abort: block busy and AEC/AF busy drop. Published results, done bits, errors and configuration outside the scope are kept. The old epoch is aborted, so it cannot clear the busy flags of a later frame (MAS §6.5.3, §9.1.1) | U `test_soft_reset` (both scopes), C G-CONT reset bursts: idle after the pulse and one frame time later, FRAME_ID kept, no late DONE |
| U1-R2 | The accepted SOF adopted `LSC_PROFILE_SEL` without re-checking validity. A begin or an abort after the selector was written made the model switch to an invalid profile and bypass LSC | The selection is re-checked at the SOF: an invalid profile keeps the previous one active and sets `PROFILE_SEL_REJECT` at every SOF while the selector differs; a later validate lets it take effect (MAS Table 31, Figure 29 p65). The reference adopts the selector at the SOF as well | U `test_lsc_selection_at_sof` |

### 9.5 U2 implementation notes (2026-10-09)

| ID | Assumption | Test |
|---|---|---|
| U2-A1 | A COMMON soft reset returns every AEC zone to untouched (min 4095), as the hard reset does. MAS p237 says it "clears published values"; whether the written flags clear too is owner query U0-Q5. | S `test_resets` |
| U2-A2 | `AF_STATUS.score_valid` is set with every publication. With EN sampled at SOF no partial frame exists; the CSR only says it "identifies published scores" (owner query U0-Q3). | S `test_af_enable` |
| U2-A3 | BPC counters: the VP clears them at the accepted SOF and writes the frame's counts at its end; a read in the middle of a frame returns 0, not the running count. A frame with `BPC_EN` = 0 is not counted (owner query U0-Q4). | S `test_bpc_counters` |
| U2-A4 | A bank freed by an abort is eligible at the next SOF even on the same core clock. In silicon the next frame's SOF reaches AWB some clocks after the abort; this loosely-timed model can place both on one edge. The same-edge rule of MAS §6.22.4.3 is applied to RELEASE. | C G-CONT `idma_axi_error` bursts |
| U2-A5 | Not modelled: the four-entry ingress context FIFO (ingress and local SOF are the same event in the VP, so context overflow cannot occur), the E2 publication delay (the bank becomes READY at the frame-end event), and the one-clock read latency of the AEC zone/histogram memories. The AWB zone read latency is modelled (review U2-R2): a read on the edge of the `AWB_ZONE_ADDR` write returns the previous address's word. `csr_latency` does not bound the core clock, so software must wait the clocks itself. | S `test_awb_commands` |
| U2-A7 | Statistics are published when the pipeline thread ends the frame. Under output backpressure this can follow the ODMA completion of that frame, whereas in silicon the AWB/AEC taps publish before the last output pixel is written. Software must wait for the publication (`stats_ready_irq`, `READY_COUNT`), as MAS §6.22.4.1 prescribes, not infer it from `ODMA_DONE`. | D `test_awb_driver` (fast CSR) |
| U2-A6 | Driver compatibility reader `fx1_isp_read_awb_global` drains every READY result and returns the newest; with none ready it returns the result it read last, so code written for the previous map keeps its semantics. `FX1_ISP_ENODATA` before any result and after a soft reset; `FX1_ISP_EBUSY` while the caller holds a lease. | D `test_awb_driver` |

Test data that changed with U2 (statistics only; no NV12 file changed, the
4K expected SHA-256 and the platform fixture CRCs are unchanged):

- Every statistics script now reads AWB through ownership: `AWB_RESULT_STATUS`
  and `AWB_DROP_COUNT`, then each READY result acquired, read and released in
  completion order. Multi-frame vectors and G-CONT bursts of three frames
  therefore show the two oldest results and count the later frames as drops.
- Every vector reads `BPC_NUM_*` = 0 when BPC is off, and
  `bpc_dynamic_requested_64x32` now reports its real counts.
- Platform fixture: the statistics profile uses a 1×1 AWB grid instead of the
  0×0 "global only" grid, which is illegal since CSR/MAS 07/10. FRAME_IDs,
  context IDs and CRCs are unchanged.
- The `gtm_manual_qhd` profile of `make_profile.py` keeps its 0×0 AWB grid on
  purpose: it now exercises the illegal-grid path (no AWB publication).

Corrections after the U2 review of 2026-10-12 (`report/ISP_CSR_MAS_U2_REVIEW_2026-10-12.md`):

| ID | Defect | Fix | Test |
|---|---|---|---|
| U2-R1 | When a lease was found lost (a COMMON soft reset the driver did not issue), the compatibility reader still returned its cached pre-reset result | The driver drops the cache together with the lease. A reset issued behind the driver while it holds no lease stays invisible to it; whoever resets the ISP must call `fx1_isp_soft_reset` or `fx1_isp_init` (header) | D `test_awb_driver`: compatibility reader `ENODATA` right after the lost lease |
| U2-R2 | The driver assumed a CSR transaction lasts a core clock. With CSR accesses shorter than the clock, it read `READ_VALID` on the ACQUIRE edge, returned `ENODATA` and lost the lease it held | The driver waits one core clock (`delay_cycles`) after ACQUIRE, after RELEASE (so the next SOF can use the bank) and after every result-memory address write (AWB and AEC zones, AEC histogram). The model now has the AWB zone read latency | `driver_tlm_fast_csr` (core 10 ns, CSR 1 ns: AWB driver test and the statistics vectors), S `test_awb_commands` |

### 9.6 U3 implementation notes (2026-10-12)

| ID | Assumption | Test |
|---|---|---|
| U3-A1 | The EE profile, enable and scalars are sampled at the accepted SOF. The MAS samples them at the EE window-centre SOF, which is later than ingress; the VP has one accepted SOF per frame. | U `test_ee_profiles`, P `test_frame_coherence` |
| U3-A2 | `PROFILE_WR_SEL` code 3: a table-port write is ignored but `EE_LUT_ADDR` still advances; `EE_LUT_RDATA` reads 0; a scalar write is ignored and the register keeps its readback. The MAS says only "its writes are ignored". | U `test_ee_profiles` |
| U3-A3 | The radial error event is raised at the end of a processed frame. An aborted frame raises nothing. | U `test_ee_radial_error` |
| U3-A4 | GTM: a table write while `EN` = 1 is discarded and `GTM_LUT_ADDR` still advances. | U `test_gtm_lut_gate` |
| U3-A5 | Driver: `fx1_isp_ee_load_profile` refuses (`FX1_ISP_EBUSY`) while `EE_STATUS.busy` = 1 every profile selected since EE was last seen idle (a set, review U3-R1); reading busy = 0 resets the set to the current selection. GTM software curve (MAS §9.2): `fx1_isp_gtm_manual_begin` (step 1) records `IDMA_FRAME_COUNT` and the frames in flight; `fx1_isp_load_gtm_lut` returns `FX1_ISP_EAGAIN` until the pipeline has taken `in_flight + 1` more frames, so a frame queued after step 1 has had its SOF and consumed any pending automatic curve (step 2), then loads, verifies (`FX1_ISP_EIO`) and restores `EN` (review U3-R2). `IDMA_FRAME_COUNT` counts every frame whose end can have built a curve, also one whose output failed, and only `i_rst_n` clears it: the load proceeds at once only when it is 0 and nothing is queued (review U3-R3), which also covers a re-initialisation without `i_rst_n`. "Nothing queued" matters on its own: the first frame after `i_rst_n` may already run on a manual curve, reading the read bank live, while `IDMA_FRAME_COUNT` is still 0 (review C1). | D `test_ee_driver`, `test_gtm_driver` |

Test data that changed with U3:

- New vector `ee_profiles_3frames_64x48`: three profiles with distinct content, selected 0, code 3, then 2.
- G-CONT `cont_100`: the EE delta now programs a random profile and selects a random code, drawn from a separate random stream; base configuration loads unity tables into all three profiles. 42 expected frames changed. With every profile choice forced to 0 (same draws), all 155 expected files equal the post-U2 data, so every change comes from the profile choice (`output/u3_attribution/attribute_sequence_u3.py`).
- No other vector, block vector, 4K vector or fixture data changed: existing profiles program profile 0 between frames, which behaves as before.

Corrections after the U3 review of 2026-10-12 (`report/ISP_CSR_MAS_U3_REVIEW_2026-10-12.md`):

| ID | Defect | Fix | Test |
|---|---|---|---|
| U3-R1 | The driver kept only the last two selections: switching 2 → 0 → 1 while a frame used profile 2 let a load into profile 2 through and changed that frame | The driver keeps the set of profiles selected since EE was last seen idle and refuses any of them while EE is busy | D `test_ee_driver` (two switches in one busy period: profile 2 and the intermediate profile refused, the frame equals the golden) |
| U3-R2 | `fx1_isp_load_gtm_lut` skipped MAS §9.2 step 2: an automatic curve built at the end of the previous frame was still pending and replaced the software curve at the next SOF | Step 1 is `fx1_isp_gtm_manual_begin`; the load returns `FX1_ISP_EAGAIN` until a frame started after step 1 has completed, then loads, verifies and restores EN. The API now returns `int` (was `void`) | D `test_gtm_driver` (auto frame, refused, one frame, load, the curve survives two more SOFs; discarded writes detected) |
| U3-R3 | The cold-state test used `ODMA_FRAME_COUNT` and a snapshot taken at `fx1_isp_init`: a first automatic frame whose output failed (lost) left a curve pending with no successful output, and a re-initialisation without `i_rst_n` reset the snapshot; both loaded at once and lost the curve at the next SOF | Cold state and the hand-over wait use `IDMA_FRAME_COUNT` (frames the pipeline took, kept by soft reset): cold only when it is 0 and nothing is queued | D `test_gtm_driver` (first frame lost, warm init; normal and fast CSR) |

### 9.7 U4 implementation notes (2026-10-12)

| ID | Assumption | Test |
|---|---|---|
| U4-A1 | A frame whose active raster does not match the published set (started before the derivation of its key finished, or after a reset) is not scaled. The MAS states this for a frame started after an abort ("is not scaled") and asks software to complete the configuration before starting a frame otherwise. | U `test_resizer_publication` (reset set), pipeline `resizer_applies` |
| U4-A2 | A frame wider than the ODMA Y/UV stride captured at arm is refused with `DMA_ERR.ALIGN` (no `IRQ_CFG_ERROR`, U1-A2) and discarded, nothing written. The MAS only asks software to size buffers for the largest mode. | D `test_resizer_switch` |
| U4-A3 | The derivation runs whatever `COMMON_CTRL.isp_en`; DEC-20 is kept: an ODMA armed while `isp_en` = 0 sees 0 × 0. | D `test_isp_en_trap` |
| U4-A4 | Edges: a key written on edge w is stable from w + 1; several writes before one edge give one key, saved at its E0 (review U4-R3). A scaling set is published at E35 = E0 + 35 and seen from the next edge; E0 = max(key stable, dividers free). A pass-through set is published on the edge after its key is stable, whether or not a derivation runs, and a derivation that completes later does not override it (review U4-R2). A derivation whose key changed ends at its E35 unpublished, and the next one starts there, so publication can take two derivations after the last change. Reset: the reset set reads 1920 × 1080 (the CSR reset values, unscaled, no key); nothing starts before the end of the soft-reset pulse, which then uses the live key, also one written during the pulse; after `i_rst_n` from edge 0. | U `test_resizer_publication` |
| U4-A5 | Frame retention and the drain rule hold by construction: the pipeline thread finishes a frame before it accepts the next SOF, so frames never overlap in the Resizer. The overlap of unchanged configurations in the decimators is not modelled (timing only). | D `test_resizer_switch` |

Driver: `fx1_isp_start` waits 80 core clocks (two derivations plus margin,
review U4-R1) after `ISP_EN` before it reads `RESIZER_OUT_W/H` and arms;
`fx1_isp_soft_reset` waits 80 more after the pulse. The tests and `tools/run_raw.cpp` that drive registers directly wait
the same before the first frame after a configuration change. No vector,
sequence, 4K or fixture data changed: every profile writes `RESIZER_CTRL`
with `updated` set, and configuration precedes the frames.

Corrections after the U4 review of 2026-10-12 (`report/ISP_CSR_MAS_U4_REVIEW_2026-10-12.md`):

| ID | Defect | Fix | Test |
|---|---|---|---|
| U4-R1 | The driver waited 40 clocks, one derivation; a key changed while the dividers worked on a previous key needs two, so `fx1_isp_start` read the previous set (wrong `EINVAL`, or a stale cached geometry) | The driver, `run_raw` and the tests wait 80 clocks (2 × 36 plus margin) | D `test_resizer_waits` (key changed during a derivation, stride sized for the last mode and for the largest; normal and fast CSR) |
| U4-R2 | Pass-through waited for a running derivation to end | Event-ordered controller: the pass-through set is due on the edge after its key is stable, independent of the dividers; the derivation's completion is dropped | U `test_resizer_publication` (disable during a derivation, SOF before and after) |
| U4-R3 | The controller saved a key before its E0: two writes before one edge, or a key written during the reset pulse, derived an obsolete key first | The key is saved only when its E0 comes; a key written during the pulse is stable from the end of the pulse | U `test_resizer_publication` (two writes before one edge; scaling and pass-through keys written in the pulse) |

### 9.8 U5 implementation notes (2026-10-12)

| ID | Assumption | Test |
|---|---|---|
| U5-A1 | `FRAME_COUNTER` counts core clocks from the release of `i_rst_n` (0 on the release edge), modulo 2^32, and reads 0 while `i_rst_n` is asserted. Neither soft reset touches it: it is the time base of the timestamps software writes, and the MAS calls it free-running. It is computed at the read edge (no clock event). The C++ `csr_read` without a cycle returns the count at the last edge the control unit processed; the TLM debug transport reads it at the current time. | U `test_time_base`; D `test_frame_counter`, `test_frame_counter_wrap`, `test_metadata_resets` |
| U5-A2 | MAS Table 183 has no metadata row. `IDMA_BUF_TAG/TS` are configuration: kept by both soft resets. The published `ODMA_BUF_*` metadata is DMA state: cleared by both soft resets with the buffer ownership bits (no buffer stays `DONE` to claim it). `i_rst_n` clears all of them. | D `test_metadata_resets` |
| U5-A3 | A frame abandoned upstream (IDMA AXI error, frame dropped by the pipeline) after the ODMA took its buffer: the ODMA sets the buffer's `FREE` bit back, keeps the rotation, and publishes no `DONE` and no metadata, so the next frame uses the same buffer (MAS §9.1.1: "processed as if it had never existed"). MAS Table 156 names only software as the setter of `FREE`; the alternative (the buffer stays taken and software frees it, as after an ODMA AXI error) is an owner question. Software sees the same `FREE` state as before U5, so the driver is unchanged. | D `test_axi_read_error` (32 rows) |
| U5-A4 | Time points. Capture: the IDMA takes the buffer's tag and timestamp when it arms it (with the geometry), before its first read; later writes reach the next use of the buffer. `SOF_TS`: the edge on which the ODMA accepts the first luma line of the frame, before its first write (also the edge on which `FREE` is cleared). `EOF_TS`: the edge on which the ODMA accepts the last line of the frame (both planes), before that line's writes. Publication (metadata, then `DONE`, same edge) follows when the last write response returns. | D `test_metadata_snapshot_and_points` (exact edges from the bus log) |
| U5-A5 | Overflow condition: ODMA enabled, the first line of a frame waiting, and the head buffer (rotation order) not free; MAS "no output buffer is free" is read as the buffer the unit waits for. The level is released when the condition ends (buffer freed, ODMA disabled, reset) and the bit then stays set until W1C. `IRQ_ODMA_OVERFLOW` is raised on each rising edge of the condition. | D `test_overflow_lossless`, C `test_cont_tlm` (overflow) |
| U5-A6 | Driver: `fx1_isp_queue_frame_meta` writes the tag and timestamp before `IDMA_BUF_VALID`; `fx1_isp_next_done_meta` reads the metadata before it returns the buffer (`DONE` W1C). The older queue calls do not write the tag registers, so their frames carry whatever the registers hold (0 after reset, or the buffer's previous tag). | D `test_metadata_driver` |

Timestamps are VP core-clock edges with the parameters of the run
(`core_period`, memory latency): they show order and latency in the model,
not silicon performance.

### 9.9 U6 notes (2026-10-12)

| ID | Assumption | Test |
|---|---|---|
| U6-A1 | Driver `fx1_isp_dma_reset` (DMA soft reset while streaming): what it keeps and changes is listed below the table. | D `test_dma_reset_driver`, `test_dma_reset_stats`, `test_dma_reset_gtm` (normal and fast CSR); platform `fx1_smoke_isp_recovery` E, `fx1_smoke_isp_mas0710` B |

U6-A1 in detail:
- **Keeps:** the enables, the AWB lease and the cached AWB result, the EE
  driver state and `COMMON_IRQ_STATUS` (U1-A1).
- **Clears:** `DMA_IRQ_STAT` and `DMA_ERR`, and restarts both rotations at 0.
- **AEC zones:** marked untrusted only if a frame in flight had not
  completed, because only such a frame can have overwritten them (MAS
  §9.1.1). "Completed" means the `ODMA_BUF_DONE` bit of that frame's own
  output (queue position k owns output `out_done` + k). A DONE bit of any
  other buffer is not counted (review U6-R3). An idle reset, or one that
  drops only DONE frames, keeps the trust state (review U6-R1).
- **DONE acknowledgement (all callers of `fx1_isp_next_done*`):** a W1C on
  the core clock on which the hardware sets DONE loses (hardware set wins).
  This is possible when a CSR access is shorter than a core clock. The
  driver reads the bit back and clears it again one core clock later, so a
  collected buffer never keeps a stale DONE that would make it look complete
  when queued again (review U6-R3).
- **GTM hand-over:** a pending one stops counting the dropped frames. If its
  fence is not reached, it waits for one frame taken after the reset
  (`IDMA_FRAME_COUNT` + 1). A dropped frame builds no curve, so that frame's
  SOF consumes any pending curve. With no frame taken since `i_rst_n` the
  load is cold (review U6-R2).
- **FRAME_ID:** an aborted frame keeps the number it was given (MAS §9.1.1,
  "FRAME_ID skips one"). A first platform run that aborted the only frame in
  flight observed the skip (AWB FRAME_ID 9, then 11). The final test aborts
  the second of two queued frames, and checks only that results after the
  reset are newer than the leased one.

Acceptance baseline (`docs/evidence/raw_regression_baseline.json`) regenerated in U6.
- **Frames:** the NV12 checksums of all 308 frames are unchanged.
- **Statistics:** the readout checksums of all 116 runs changed with the U2
  rules (AWB ownership and drops, BPC counters, AEC untouched zones).
- **Attribution:** compared read by read with the previous run, every
  difference has one of those causes and none is unexplained.
- The previous baseline was from before the revision. With it,
  `fx1_isp.regression.raw` (registered only with
  `-DFX1_ISP_RAW_INVENTORY`) would have failed since U2.

Platform coverage of the revision (plan U6 item 3):
- `fx1_smoke_isp_mas0710`: AWB ownership with drops and reuse, frame
  metadata, an AWB lease across a DMA reset, Resizer from the first frame and
  a mode switch. Its expected values come from the reference through
  `gen_fixture.py`.
- `fx1_smoke_isp_recovery` phase E and tag checks in every phase.

The questions for the owner (§9.3–§9.8) are batched in the workspace
document `plan/alg/OWNER_QUERIES_CSR_MAS_0710_2026-10-12.md` (23 questions).
Until they are answered, the acceptance covers the VP contract described
here, not silicon behaviour. In particular the LSC `LOAD_ADDR` terminal value
is not asserted at the maximum mesh.

