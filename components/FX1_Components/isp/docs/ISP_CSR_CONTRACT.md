# FX1 ISP VP — CSR behaviour contract (M1)

This is the register-level behaviour the model implements. The map itself
(offsets, fields, reset values) is `include/fx1_isp/fx1_isp_csr.h`, generated
from the pinned CSR spreadsheet. Decision and issue IDs refer to
[ISP_DECISIONS_AND_DISCREPANCIES.md](ISP_DECISIONS_AND_DISCREPANCIES.md).

## 1. Access types (HAS Table 8-1, Table 7-10)

| Type | Software write | Software read | Hardware |
|---|---|---|---|
| RW | Bits in strobed lanes take the written value | Stored value | May update (e.g. LUT address auto-increment) |
| RO | Ignored | Hardware value | Drives the value |
| W1C | 1 clears, 0 no effect | Stored value | Sets the bit |
| W1S | 1 sets, 0 no effect | Current state, not the last write | Clears the bit |
| W1SC | 1 pulses a command | Always 0 | — |

General rules:

- Every access completes with OKAY. Unmapped words read 0 and ignore writes.
  Reserved bits read 0 and ignore writes.
- Byte strobes are honoured per lane. A write with no strobes changes nothing
  and triggers no side effect (no LUT write, no auto-increment, no command).
- Several W1C/W1S bits in one write act independently.
- **Hardware set wins:** if hardware sets a W1C bit in the same core-clock cycle
  as a software write-1-to-clear, the bit stays set. The cycle is
  `floor((sc_time_stamp() + annotated delay) / core_period)`.
- **Level-held bits** (`DMA_ERR.IDMA_UNDERRUN`, CSR-06): while the condition
  persists a W1C write does not clear the bit. Once the condition drops the
  bit stays set until software clears it. The derived interrupt bits are set
  on the rising edge of the condition only.
- Reserved bits inside declared fields are tied to zero:
  `DMA_IRQ_EN[0]`, `DMA_IRQ_STAT[0]` (SPEC-05), `DMA_ERR[5]`.

## 2. TLM adapter (CSR-23)

The CSR socket models the AXI4-Lite slave behind a CPU-side bridge:

| TLM access | Behaviour |
|---|---|
| 4 bytes, any address | Word `addr & ~3`, all lanes; address bits [1:0] are ignored |
| 1–3 bytes | Byte lanes `addr[1:0] … addr[1:0]+len-1` of word `addr & ~3` |
| 1–3 bytes crossing a word boundary, or length > 4 | `TLM_BURST_ERROR_RESPONSE`, no effect |
| Address ≥ 0x10000 | `TLM_ADDRESS_ERROR_RESPONSE` (routing error, outside the aperture) |
| Null data pointer (read/write) | `TLM_GENERIC_ERROR_RESPONSE`, no effect; debug read returns 0 bytes |
| Non-null byte-enable pointer with zero length | `TLM_BYTE_ENABLE_ERROR_RESPONSE`, no effect |
| Byte-enable array | Honoured, `TLM_BYTE_ENABLED` per data byte, repeating over its length |
| `TLM_IGNORE_COMMAND` | OK, no effect |

An access takes effect at its annotated time: the target waits out a non-zero
incoming delay before applying it (synchronise-on-access, see
[ISP_MODEL_INTERFACE.md](ISP_MODEL_INTERFACE.md) §3), then returns
`csr_latency` as the delay. DMI is refused
(registers have side effects). `transport_dbg` supports reads only; reads have
no side effects, so a debugger sees exactly what software would read.

While `rst_n` is low, CSR writes are ignored and reads return reset values.

## 3. Reset

### 3.1 External reset (`rst_n` low, HAS §6.5)

All registers return to their reset values, all level holds and pending
commands are cleared, and any soft-reset window is cancelled. The `irq` output
is low while in reset.

LUT contents after reset are undefined (MAS §6.5.2, Table 183). The model
fills Gamma, EE and the GTM banks with a fixed, deterministic poison pattern,
`((i * 0x9E37) ^ 0xA5A5) & mask`; the Python reference uses the same pattern.
A block enabled without its table therefore gives a visibly wrong picture.
Software must load a table before enabling its block. LSC profile validity is
cleared. (CSR-16 is superseded.)

### 3.2 Soft reset (MAS §6.5.3, Table 183 p316-317)

`COMMON_CTRL.soft_rst` and `DMA_CTRL.soft_reset` are W1SC: they read 0, and a
write starts or restarts a **15-core-clock** pulse. The register effects
happen at the write; the engines stay in reset until the pulse ends. The two
sources have different scopes.

| | `COMMON_CTRL.soft_rst` | `DMA_CTRL.soft_reset` |
|---|---|---|
| Scope | Pixel pipeline and both DMA engines | DMA engines and the frame in flight |
| RW configuration (incl. `idma_en/odma_en`, `isp_en`) | Preserved | Preserved |
| Gamma, EE, GTM, LSC contents; LSC validity | Preserved | Preserved |
| `IDMA/ODMA_FRAME_COUNT`, `AEC/AWB/AF_FRAME_ID` | Preserved; the aborted frame's number is skipped | Preserved; skipped |
| Pending `AEC_CTRL.commit` | Preserved, consumed at the next accepted SOF (owner answer Q4) | Preserved |
| Buffer ownership, frame in flight | Cleared / discarded | Cleared / discarded |
| `DMA_IRQ_STAT`, `DMA_ERR` (incl. held bits) | Cleared | Cleared |
| `COMMON_IRQ_STATUS`, `COMMON_STATUS`, statistics results and done flags, other pending commands, LSC load in progress, 2DNR estimate | Cleared | Kept, except `COMMON_STATUS.busy`, which clears because the pipeline is flushed (assumption U1-A1 for `COMMON_IRQ_STATUS`) |

Rule used by the model: every RO/W1C/W1S bit in the scope returns to its reset
value, except for the preserved registers listed above. For the DMA scope, the
registers in scope are those from `DMA_CTRL` (0x3000) up.

During the pulse, software writes still update RW configuration, but their
W1S/W1C effects are dropped, because the state they target is held in reset.
For example, arming `IDMA_BUF_VALID` inside the pulse has no effect.

## 4. Interrupts (HAS §9.2)

```
irq = |(COMMON_IRQ_STATUS & COMMON_IRQ_EN) | |(DMA_IRQ_STAT & DMA_IRQ_EN)
```

Status bits latch whether or not their enable bit is set. The line stays high
while any enabled status bit is set.

| COMMON_IRQ_STATUS bit | Set by |
|---|---|
| `frame_done_irq` | Output frame complete: last write response of both planes (M2) |
| `error_irq` | Rising edge of any bit in `LSC_ERROR[5:0]`, `BPC_STATUS.cand_reject_ovf`, `EE_STATUS.error`, `DMA_ERR[4:0]` (DEC-15, including `IDMA_UNDERRUN`) |
| `stats_ready_irq` | Every statistics publication (AEC, AWB, AF), also when that block's done bit is still set (M3-A4); a backdoor set of a done bit also raises it |

`DMA_IRQ_STAT` bits are set by the DMA engines (M2). Bit 0 is `CFG_ERROR`
(U1, SPEC-05 superseded): a refused IDMA or ODMA start sets it together with
`DMA_ERR.ALIGN`, which also raises `error_irq`. A geometry mismatch found
after the start sets `DMA_ERR.ALIGN` only (U1-A2).

## 5. Register-specific behaviour implemented in M1

| Register | Behaviour |
|---|---|
| `COMMON_STATUS.error` | Live OR of the DEC-15 error flags; clears when those flags are cleared |
| `COMMON_STATUS.frame_done` | Set together with `frame_done_irq`; cleared at the next accepted SOF (DEC-17) |
| `COMMON_CTRL.frame_start` | Pending flag, cleared at the next accepted SOF; does not start or block frames (DEC-16) |
| `GAMMA_LUT_ADDR/DATA/RDATA` | A DATA write stores the merged 12-bit value at ADDR, then ADDR increments modulo 256. DATA reads back the last value written; RDATA reads the table at ADDR (CSR-21) |
| `EE_LUT_CTRL/ADDR/WDATA/RDATA` | `lut_sel` picks the bank of the profile `EE_CTRL.profile_wr_sel` names; ADDR increments modulo 64, also for a write ignored under code 3; RDATA reads 0 under code 3 (U3-A2). Contrast banks (2, 3) use `addr & 31` |
| `EE_ALPHA` … `EE_RADIAL_GAIN_23` | Written into the profile `profile_wr_sel` names; code 3 ignores the write and keeps the readback. Readback = last value written through the register. Each profile resets to the CSR values (MAS p189) |
| `EE_CTRL.en`, `.profile_sel` | Sampled at the accepted SOF with the selected profile's scalars (code 3 selects profile 0); the profile's tables are read live row by row (MAS §6.18.4.1) |
| `EE_STATUS.error` | W1C; set at the end of every frame EE processes (EE on, at least 5 × 5) with radial gain on and a radial gain of the frame's profile above 1.5 (MAS §9.2); not a level of the live registers |
| `GTM_LUT_RDATA` | Reads the table at `GTM_LUT_ADDR`; index > 64 reads 0 |
| `RESIZER_OUT_W/H` | The published set (MAS §6.20.4.3): for the key {active size, mode, enable}, active size = `floor((W − S_H)/2)·2 × floor((H − S_V)/2)·2` with `S_H = pattern[0]`, `S_V = pattern[1]`; the mode's target if enabled and it fits, otherwise the active size (CSR-09). A key written on edge w is stable from w + 1; writes before one edge give one key, saved only when its E0 comes; a scaling set is published at E35 (visible from w + 37 when the dividers are free), a pass-through set on the edge after the key is stable, even while a derivation runs; a key changed during a derivation is derived when the dividers are free (at that E35), and the stale result is never published. Reset (i_rst_n, COMMON soft reset): the reset values 1920 × 1080 (unscaled); the live key at the end of the pulse, also one written during it, is derived from there |
| `RESIZER_CTRL.updated` | No effect (MAS §9.2; DEC-24/26 superseded) |
| `AF_CTRL.search_start/abort` | No hardware behind them: stay 1 after a write until reset or soft reset (CSR-07) |

## 6. Commands and their milestone

| Register | Behaviour | Status |
|---|---|---|
| `COMMON_CTRL.isp_en` | Gates the pipeline (accepted SOF and every line) and the output geometry the ODMA captures (DEC-20) | M2 |
| `DMA_CTRL.idma_en/odma_en/max_burst_m1` | Engine enables, effective at frame boundaries (M2-A2); burst length captured at arm | M2 |
| `IDMA_BUF_VALID`, `ODMA_BUF_FREE`, `ODMA_BUF_DONE` | Ownership handshake of HAS §6.25.10; `FREE` cleared when the ODMA starts the frame (MAS Table 159; SPEC-07 superseded) | M2, U5 |
| `IDMA_BUF_TAG/TS0..3`, `ODMA_BUF_TAG/TS/SOF_TS/EOF_TS0..3`, `FRAME_COUNTER` | Frame metadata (MAS Table 160): input tag/timestamp captured at frame start, published with the output buffer at completion; `FRAME_COUNTER` core clocks since `i_rst_n` release, modulo 2^32 (U5-A1..A5) | U5 |
| `LSC_LOAD_CTRL`, `LSC_COEF_DATA`, `LSC_PROFILE_SEL` | Load sequencer with a staging buffer. The command bits self-clear, with priority abort > validate > begin. Begin is rejected (`LSC_ERROR`) while a load is busy, for an invalid destination or geometry (2..128 nodes per axis, not above the active size), or for the active profile, valid or not (MAS Table 31, owner Q5). Begin invalidates its destination at once. Validate commits only with the exact coefficient count; otherwise `COEF_COUNT` is raised and the destination stays invalid, as after an abort. A coefficient above 4.0 sets `COEF_RANGE`. `LSC_MESH_NODES` writes invalidate every profile. `LSC_PROFILE_SEL` is checked when written and again at the next accepted SOF: it becomes active only if its profile is still valid then; otherwise the previous profile stays active and `PROFILE_SEL_REJECT` is set at every SOF while the selector differs (MAS Figure 29, review R2) | M3, U1 |
| `GTM_LUT_DATA` | Writes go to the read bank only while `GTM_CTRL.en` = 0, whatever `manual` says (MAS p146, U3); ADDR increments modulo 128 on every write. In auto mode the curve is built at EOF into the other bank and swapped at the next SOF | M3 |
| `AEC_CTRL.commit` | Pending (reads 1) until the next accepted SOF, which copies the shadow set (`en`, `ZONE_CFG`, `ZONE_SIZE`, `SAMPLE_CLIP`, `THRESH`) to the active set and clears the bit. A pending commit survives both soft resets and is cleared by `i_rst_n` (owner Q4, DEC-32 superseded) | M3, U1 |

With M3 every command register is consumed by a block model, so
`debug_pending_commands()` stays 0 in the integrated IP. Commands without an
installed hook (unit-level use of the control unit) are still recorded. They
are discarded by external reset and by the COMMON soft reset, together with
the state they target; the DMA-only soft reset keeps them (MAS Table 183). The soft-reset write itself is consumed by the
control unit and is never recorded. The engines re-evaluate their start
conditions after every CSR write.

## 7. Block status and registers without hardware (M5)

| Register | Behaviour |
|---|---|
| `DEMOSAIC_STATUS`, `CCM_STATUS`, `GAMMA_STATUS`, `GTM_STATUS`, `NR_2D_STATUS`, `EE_STATUS`, `CNF_STATUS`, `RESIZER_STATUS` `.busy` | 1 from the accepted SOF until the pipeline has emitted the frame's last row; it stays 1 during a stall. Cleared by abort and reset (DEC-38) |
| `OFMT_CTRL`, `OFMT_Y_STRIDE`, `OFMT_UV_STRIDE` | Storage only. `stride_en` = 1 warns and does not change the output (DEC-39) |
| `D_WDR_*`, `TNR_3D_*` | Storage only. The feature is unsupported (HAS §3.1), and enabling it warns (M5-A1) |
| `BPC_MODE`, `BPC_TEMPORAL_VAR`, `BPC_PIXEL_AGE` | Storage only: spatial BPC only, `dynamic_det_en` has no effect (MAS §6.9; DEC-28 superseded) |
| `CNF_STRENGTH`, `AWB_X/Y_BOUND_*`, `AF_ZONE_EN`, `AF_METRIC_CFG`, `AF_LUT_*`, `AF_VCM_*`, `AF_PEAK_SCORE`, `AF_FOCUS_RANGE` | Storage only: the CSR states there is no hardware behind them |

The complete per-register classification is
[ISP_CSR_TRACEABILITY.md](ISP_CSR_TRACEABILITY.md).

## 8. Statistics readout (M3)

Detail and rationale: `plan/alg/ALG_F_aec_awb_af.md`, DEC-29/30/33 and M3-A8.

| Register | Behaviour |
|---|---|
| `AEC_GLOBAL_SUM_LO/HI`, `AEC_GLOBAL_COUNT` | Channel `AEC_CHANNEL_SEL` (0 R, 1 Gr, 2 Gb, 3 B) of the last publication; stable until the next one |
| `AEC_ZONE_SUM/COUNT` (by channel), `AEC_ZONE_GREEN_OE_UE`, `AEC_ZONE_GREEN_MIN_MAX` | Zone `AEC_ZONE_ADDR` (raster order) of the single in-place memory (DEC-30), read as it is now. Before the first enabled SOF after a reset every zone is untouched: 0, min 4095, max 0 (MAS §6.21.4.3; the CSR reset value 0 of `GREEN_MIN_MAX` is superseded). From the SOF of an enabled frame: a zone already written in that frame reads its partial sums; a zone not yet written, an address beyond the grid, or any zone of an illegal grid reads the empty value (0; min 4095, max 0). After EOF the memory holds the complete frame until the next enabled SOF |
| `AEC_HIST_DATA` | Bin `AEC_HIST_ADDR` of the green histogram, in the same memory: 0 at the enabled SOF, then partial counts |
| `AWB_GLOBAL_*`, `AWB_FRAME_ID`, `AWB_RESULT_CONTEXT_ID` | Words of the leased bank (35-bit sums as `_H[2:0]` + `_L`, 23-bit count), from the edge after ACQUIRE; 0 without a lease (MAS §6.22.4) |
| `AWB_ZONE_*` | Zone `AWB_ZONE_ADDR` of the leased bank, for the grid captured with it; 0 for untouched zones, addresses beyond that grid and without a lease |
| `AWB_RESULT_CTRL` | One-clock commands, read 0: `acquire` (oldest READY bank; no-op with a lease or nothing READY, or for a bank published on the same edge), `release` (wins over acquire; no-op without a lease), `clear_report` (clears `overrun` and `AWB_DROP_COUNT` only; a loss on the same edge is kept) |
| `AWB_RESULT_STATUS` | `ready_count`, `leased` = `read_valid`, `read_bank`, `write_busy`, `overrun`, `read_num_zone_x/y` of the lease (0 without one) |
| `AWB_DROP_COUNT` | Enabled measurements skipped (no FREE bank at SOF, illegal grid), saturating at 2^32 − 1; disabled frames do not count |
| `AF_STAT_DATA` | Score of zone `AF_STAT_ADDR` (4×4, raster order); latched, coherent until the next publication |
| `*_FRAME_ID` | ID of the frame that was measured: one counter, +1 per accepted SOF since `i_rst_n`, kept by soft reset |
| `*_RESULT_CONTEXT_ID` | `*_CONTEXT_ID` as latched at that frame's SOF; cleared by soft reset |
| `AEC_STATUS` | `busy` from SOF to publication of an enabled frame; `stat_done` W1C, set per publication; `error` reads 0 |
| `AWB_STATUS.stat_done` | Set per publication (a bank becoming READY); clearing it releases no bank |
| `AF_STATUS` | `busy` while a measured frame is in flight; `frame_done` W1C, set per publication of a frame whose EN was 1 at its SOF; `score_valid` set with every publication (assumption U2-A2) |

Software protocol (HAS §6.21.9): read FRAME_ID, then the results, then
FRAME_ID again, and retry if it changed. That protects the double-buffered
globals. For zone and histogram data, FRAME_ID changes only at EOF, so it
cannot detect a read that overlaps the next frame. That read returns partial
sums of the new frame mixed with the finished one. Software must also see
`AEC_STATUS.busy` = 0 before and after the read, or hold the next input
buffer until the readout is done (ALG-STAT-02). AWB results are coherent by
construction: the leased bank is never written.
