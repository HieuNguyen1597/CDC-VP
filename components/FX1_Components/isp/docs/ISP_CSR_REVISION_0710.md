# FX1 ISP VP — CSR/MAS revision of 2026-10-07 (U0–U4)

Milestones of plan `ISP_CSR_MAS_UPDATE_PLAN_2026-10-09` covered here:
- U0: pin the new CSR map, regenerate the ABI artefacts, record what changed
  and what the model does not implement yet;
- U1: reset, LSC, commit and DMA start-error behaviour of the 07/10 contract;
- U2: AWB result banks and their driver API; AF enable, BPC counters and AEC
  untouched zones;
- U3: EE resident profiles and the GTM table write gate;
- U4: Resizer configuration derivation, publication and per-frame geometry.

Behaviour changes and their milestones are in §9 of
[ISP_DECISIONS_AND_DISCREPANCIES.md](ISP_DECISIONS_AND_DISCREPANCIES.md).
The sources are confidential and are pinned in
[ISP_SOURCE_MANIFEST.md](ISP_SOURCE_MANIFEST.md); this page carries no
spreadsheet text.

## ABI delta

| | Previous (`0980b61d…`) | 07/10 (`bda077b6…`) |
|---|---|---|
| Registers | 226 | 254 |
| Fields | 298 | 336 |
| RW / RO / W1C / W1S / W1SC fields | 192 / 75 / 20 / 9 / 2 | 201 / 101 / 20 / 12 / 2 |

No register was removed or moved. No existing field changed its bits, access
or reset. The `Registers` and `Flat Registers` sheets agree.

**New registers:**

| Group | Registers | Offsets |
|---|---|---|
| AWB result banks | `AWB_RESULT_CTRL` (W1S commands), `AWB_RESULT_STATUS` (RO), `AWB_DROP_COUNT` (RO) | `0x2468`–`0x2470` |
| DMA buffer metadata | `IDMA_BUF_TAG0..3`, `IDMA_BUF_TS0..3` (RW); `ODMA_BUF_TAG0..3`, `ODMA_BUF_TS0..3`, `ODMA_BUF_SOF_TS0..3`, `ODMA_BUF_EOF_TS0..3` (RO); `FRAME_COUNTER` (RO, core-clock time base) | `0x3100`–`0x3160` |

**New field:** `EE_CTRL.profile_wr_sel` [5:4] RW, which selects the EE
profile to be programmed.

**Masks changed** in existing registers:

| Register | Previous | 07/10 | Why |
|---|---|---|---|
| `EE_CTRL` | RW `0x0F` | RW `0x3F` | `profile_wr_sel` |
| `DMA_IRQ_EN`, `DMA_IRQ_STAT` | `0x7E` | `0x7F` | bit 0 is `CFG_ERROR`; SPEC-05 is superseded |

**Descriptions:** 58 register descriptions and 119 field descriptions changed.
Each change was reviewed against the MAS in U0. The outcome is the table in
§9 of the decisions log.

## Generator (`tools/gen_csr.py` v1.1)

- **Pin:** the SHA-256 is now `bda077b6…`, and the counts are those above.
- **Overrides:**
  - DEC-11 is now an assertion (`assert_enum`): the spreadsheet encodes the
    Bayer pattern the way DEC-11 decided, and the generator fails if a later
    revision reverts it.
  - SPEC-05 (DMA_IRQ bit 0 tied to zero) is removed.
  - CSR-DMA-ERR-5 (DMA_ERR bit 5 reads zero) is kept, with the new text
    asserted.
- **Parser:** bit names are also read in the 07/10 style, `Bit N NAME` with
  possibly several per line. Without this, `DMA_ERR`, `DMA_IRQ_*` and
  `ODMA_BUF_*` lost their bit macros.
- **Naming overrides:** two fields now give their values in prose only. The
  generator names them, and asserts the text:
  - `VP-NAMES-CSC`: `CSC_CTRL.std` values `BT601`/`BT709`;
  - `VP-NAMES-ODMA-BUF`: `ODMA_BUF_FREE/DONE` bits `BUFFER_0..3`.
- **Compatibility aliases** (`CSR0710-RENAME`): the old `DMA_ERR` bit macros
  stay as aliases of the new names, so code written against the previous map
  keeps compiling. New code should use the new names.

  | Old | New |
  |---|---|
  | `…_DMA_ERR_ERR_ALIGN_OR_GEOMETRY_BIT` | `…_ALIGN_BIT` |
  | `…_IDMA_AXI_BIT` | `…_AXI_RD_BIT` |
  | `…_ODMA_AXI_BIT` | `…_AXI_WR_BIT` |

- **Reserved fields:** `reserved; no processing effect`, `compatibility field`
  and `reserved for a future` count as having no hardware behind them.
- **Header compared with the previous one:** no macro removed, 213 macros
  added. Apart from the two mask rows above, no existing value changed.

## Not modelled yet (traceability class `deferred`)

`tools/gen_traceability.py` classified the 28 new registers as **deferred**
in U0; after U2, 25 remain.
The register file stores them: RW words keep what is written, RO words read
their reset value. Each entry names the milestone that implements it. The
generator fails if a deferred register gains a model reference without being
reclassified, so the list cannot go stale.

| Registers | Milestone |
|---|---|
| `AWB_RESULT_CTRL`, `AWB_RESULT_STATUS`, `AWB_DROP_COUNT` | U2 (done, now `model`) |
| `IDMA_BUF_TAG/TS*`, `ODMA_BUF_TAG/TS/SOF_TS/EOF_TS*`, `FRAME_COUNTER` | U5 (done, now `model`) |

No register is deferred after U5.

## Effect on the tree in U0

- **Regenerated:** schema, header, register table, traceability report, and
  the platform fixture header `fw/fx1_soc/tests/isp_stream/isp_fixture.h`.
  For the fixture header only the provenance hash changed, because it covers
  the schema; all CRCs are unchanged.
- **Tests updated for the new ABI:**
  - `tests/unit/test_control_unit.cpp`: register and field counts, and the
    seven DMA IRQ sources;
  - `tools/check_csr_header.py`: SPEC-05 removed, 254 registers.
- **Pixels and statistics unchanged.** The real-RAW regression of the 4
  presets on `khoid_outdoor1` matches the U0 baseline (every frame and
  statistics SHA-256). The U0 baseline is CDC-VP `e51b325`: 116/116 runs,
  308/308 frames.

## U1 — behaviour changes

Each row supersedes an entry of §9.1 in the decisions log. Assumptions
U1-A1..A3 are in §9.4.

| Area | Before | Now | Software impact |
|---|---|---|---|
| Soft reset pulse | 32 core clocks | 15 core clocks, W1SC; a rewrite restarts the pulse | Driver waits 15 cycles (`SOFT_RESET_CYCLES`) |
| `COMMON_CTRL.soft_rst` | Same as the DMA reset | Pipeline and both DMA engines. Clears statistics, all IRQ status, pending commands, an LSC load in progress and the 2DNR estimate | None |
| `DMA_CTRL.soft_reset` | Same as the COMMON reset | DMA block (offsets from `0x3000`) and the busy flags of the abandoned frame (pipeline, pixel blocks, AEC/AF) only. Published statistics, `COMMON_IRQ_STATUS`, LSC and 2DNR are kept | Recover a DMA fault without losing statistics |
| Both resets | Pending `AEC_CTRL.commit` dropped | Kept; consumed at the next accepted SOF. Configuration, LUTs, LSC validity, frame counters and FRAME_IDs kept | None |
| LUT RAM after `rst_n` | Gamma 0, EE `0x8000`, GTM 0 | Deterministic poison pattern (contents undefined in the MAS) | Load every LUT before enabling its block |
| LSC mesh | 2..32 nodes | 2..128 nodes per axis, not above the active width/height | Larger meshes accepted; meshes larger than the image refused |
| LSC load | Begin into any profile; begin kept the old validity | Begin into the active profile is rejected; begin invalidates the destination; abort or count mismatch leaves it invalid; a geometry change invalidates all. The selection is re-checked at the SOF: an invalid profile keeps the previous one and sets `PROFILE_SEL_REJECT` | Boot: load profile 1, validate, select 1; do not load into a selected profile before the SOF |
| CCM, CNF | `updated` commit gate | Captured at the accepted SOF; `updated` has no effect | Write the coefficients before the frame starts |
| Resizer | `updated` commit gate | Unchanged until U4 | None yet |
| `DMA_IRQ` bit 0 | Tied to zero | `CFG_ERROR`, set with `DMA_ERR.ALIGN` on a refused IDMA/ODMA start | The driver enables it in `DMA_IRQ_EN` |

**Old profiles:** a `.csrw` profile written under the previous rules does
not work on the U1 model. Such a profile begins an LSC load into the active
profile 0, which is now rejected, and enables EE without loading its tables,
so EE reads the poison pattern. Regenerate the profile with
`tools/make_profile.py`. This affects the profiles shipped in SDK
VP_FX1_V3.0. Regenerated profiles reproduce the V3.0 NV12 goldens exactly.

**Driver (`driver/src/fx1_isp_drv.c`):** mesh ceiling 128, reset wait 15
cycles, new `DMA_ERR` names, `CFG_ERROR` enabled. The API is unchanged.

**Evidence:**
- ISP ctest 21/21.
- Ten mutants, one per U1 rule, each undoing the rule in the model. Every
  one is killed by a functional test.
- Changed test data is attributed rule by rule (decisions log §9.4). The 4K
  expected SHA-256, `full_chain_*` and the platform fixture CRCs are unchanged.

## U2 — statistics

Each row supersedes an entry of §9.1 in the decisions log. Assumptions
U2-A1..A6 are in §9.5.

| Area | Before | Now (MAS 07/10) | Software impact |
|---|---|---|---|
| AWB results | One in-place zone memory, globals replaced at EOF, read any time | Two banks: FREE → WRITING (admitted at an enabled SOF, bank 0 first) → READY (frame end) → LEASED (`acquire`, oldest first) → FREE (`release`). Globals, zones, FRAME_ID and CONTEXT_ID read the leased bank only, from the clock after ACQUIRE; 0 without a lease. READY/LEASED banks are never written | Acquire, read, release. A reader that never releases sees the two oldest results |
| AWB loss | — | No FREE bank, or an illegal grid, at an enabled SOF: the measurement is skipped, `overrun` set, `AWB_DROP_COUNT` +1; video continues. `clear_report` acknowledges | Release promptly; check `fx1_isp_awb_report` |
| AWB grid | 0 × 0 meant "global only" | Illegal: 1 ≤ nx ≤ min(64, W), 1 ≤ ny ≤ min(32, H) | Use a 1 × 1 grid for a global result |
| AWB reset | Results cleared by both soft resets | DMA-only reset aborts the measurement in flight, keeps READY/LEASED banks and reporting; COMMON and hard reset clear ownership and reporting | None |
| AF enable | Read live per row; publication iff EN at EOF; `score_valid` = EN from SOF to EOF | Sampled at SOF: a frame started enabled is published even if EN is cleared during it; one started disabled is ignored. `score_valid` with every publication | Clearing EN no longer discards the frame in progress |
| BPC counters | Written at EOF when enabled; 0 with `dynamic_det_en`; kept while disabled | Cleared at every SOF, counted in an enabled frame (14-bit); `dynamic_det_en` has no effect and no longer warns | Counters read 0 after a disabled frame |
| AEC zones after reset | `GREEN_MIN_MAX` read 0 | Untouched: min 4095, max 0 | None |

**Driver (`driver/`):**
- New API: `fx1_isp_awb_acquire`, `fx1_isp_awb_read_global`,
  `fx1_isp_awb_read_zones`, `fx1_isp_awb_release`, `fx1_isp_awb_report`.
- `fx1_isp_read_awb_global` keeps its meaning (newest result): it drains the
  ready results through the new API and caches the newest one.
- `fx1_isp_dev` grows by the lease and the cache, so callers must rebuild.
- The driver waits one core clock after ACQUIRE, after RELEASE and after each
  result-memory address write; CSR accesses may be shorter than a core clock
  (review U2-R2).

**Model interface:** `control_unit::csr_read(addr, cycle)` reads hardware
state at an edge. The TLM target passes the access cycle; the overload
without a cycle reads the settled state.

**Evidence:**
- ISP ctest 22/22, including `driver_tlm_fast_csr` (core 10 ns, CSR 1 ns).
- New directed tests cover MAS Table 140: LEASE, FULL, COMMAND, REPORT, GRID,
  RESET and PUBLISH.
- The driver is checked over TLM for every vector that reads AWB, plus a
  dedicated ownership test.
- Mutation and RAW evidence are in `report/ISP_CSR_MAS_U2_2026-10-09.md`
  (workspace).

## U3 — EE profiles, GTM table

Each row supersedes an entry of §9.1 in the decisions log. Assumptions
U3-A1..A5 are in §9.6.

| Area | Before | Now (MAS 07/10) | Software impact |
|---|---|---|---|
| EE storage | One set of scalars and tables | Three resident profiles, each with `EE_ALPHA` … `EE_RADIAL_GAIN_23` and the four tables; `EE_CTRL.profile_wr_sel` steers register and table-port writes (code 3 ignored); `EE_LUT_RDATA` reads that profile; a scalar reads back the last value written through it | Program a profile with full 32-bit writes; a partial write merges bytes possibly of another profile |
| EE activation | Enable and scalars live per row | `EE_CTRL.en`, `profile_sel` (code 3 = profile 0) and the profile's scalars sampled at SOF; tables stay live, a write to the in-use profile's table tears the frame | Load an unused profile, then select it; do not rewrite the in-use profile's tables while streaming |
| EE radial error | Level of the live registers, re-asserted at once | Event per processed frame (EE on, ≥ 5 × 5, radial on, a gain of the frame's profile > 1.5); W1C, recurs while programmed | Clearing it while the gain stays illegal returns at the next frame |
| GTM table | Writable while disabled or MANUAL | Writable only while `EN` = 0 | Load with `EN` = 0, `MANUAL` = 1, then set `EN` |

Existing single-profile programs keep their behaviour: after reset both
selectors are 0, so writes and the datapath use profile 0.

**Driver:**
- New API: `fx1_isp_ee_load_profile` (full-width writes, table read-back,
  `FX1_ISP_EBUSY` for any profile selected since EE was last seen idle) and
  `fx1_isp_ee_select`.
- `fx1_isp_load_ee_table` writes the profile `profile_wr_sel` names.
- GTM software curve per MAS §9.2: `fx1_isp_gtm_manual_begin`, then
  `fx1_isp_load_gtm_lut`, which returns `FX1_ISP_EAGAIN` until the pipeline
  has taken a frame queued after the begin (`IDMA_FRAME_COUNT`, so a lost
  output still counts). It loads at once only when no frame was taken since
  `i_rst_n`. Its return type changed from `void` to `int`.

**Evidence:**
- ISP ctest 23/23 (new `fx1_isp.unit.ee_profiles`).
- Mutation and RAW evidence are in `report/ISP_CSR_MAS_U3_2026-10-12.md`
  (workspace).

## U4 — Resizer configuration and per-frame geometry

Assumptions U4-A1..A5 are in §9.7 of the decisions log.

| Area | Before | Now (MAS 07/10) | Software impact |
|---|---|---|---|
| Configuration | `RESIZER_CTRL.scale` committed through `updated`; `RESIZER_OUT_W/H` derived at once | Key {active size, mode, enable}; a stable key is derived in E0–E35 and the set published atomically; pass-through on the next edge; a key change during a derivation re-derives; `updated` has no effect | Wait up to 2 × 36 clocks after a change before reading `RESIZER_OUT_W/H` or starting a frame that depends on it |
| Frame | The pipeline read the committed scale at SOF | The frame takes the set published before its SOF and keeps it; a mode written during a frame applies to the next | None |
| ODMA | Geometry captured at arm; a frame of another size discarded with `ALIGN` (M2-A4) | Each frame written with its own size, no re-arm; strides checked at arm; a frame wider than a stride refused | Size strides for the largest mode |
| Reset | — | COMMON soft reset and `i_rst_n` publish the reset set; derivation restarts after the pulse; a frame started earlier is not scaled | Wait for the pulse and a derivation after a soft reset |

**Driver:** `fx1_isp_start` and `fx1_isp_soft_reset` wait 80 core clocks for
the published set (two derivations: a key changed during one is derived after it).

**Evidence:**
- ISP ctest 23/23.
- Mutation and RAW evidence are in `report/ISP_CSR_MAS_U4_2026-10-12.md`
  (workspace).

## U5 — DMA metadata and time base

Assumptions U5-A1..A6 are in §9.8 of the decisions log.

| Area | Before | Now (MAS 07/10) | Software impact |
|---|---|---|---|
| Metadata | Not modelled (registers stored only) | The IDMA captures `IDMA_BUF_TAG/TS[n]` of the buffer it starts; the frame carries them through the pipeline; the ODMA publishes them with `SOF_TS`/`EOF_TS` in `ODMA_BUF_*[m]` of the buffer it completes. Input and output indices need not match. Nothing is published for an aborted or failed frame | Write the tag and timestamp before `IDMA_BUF_VALID`; read the output metadata before freeing the buffer |
| `FRAME_COUNTER` | Read 0 | Free-running 32-bit core-clock count since `i_rst_n` release, wraps modulo 2^32, not reset by soft resets | Compute spans modulo 2^32 |
| `ODMA_BUF_FREE` | Cleared when the buffer is full (SPEC-07) | Cleared when the ODMA starts the frame in it | `FREE` = 0 during the write; the recycle loop is unchanged |
| `ODMA_OVERFLOW` | Set once per stall episode (DEC-18) | Held while the stall lasts; W1C clears it only after the stall; IRQ once per stall | Clear the status after freeing a buffer |

**Driver:** `fx1_isp_queue_frame_meta(dev, tag, ts, &seq)`,
`fx1_isp_next_done_meta(dev, &index, &seq, &meta)` and
`fx1_isp_frame_counter(dev)`. The older calls are unchanged.

**Evidence:**
- ISP ctest 23/23.
- Mutation and RAW evidence are in `report/ISP_CSR_MAS_U5_2026-10-12.md`
  (workspace).

## U6 — integration, regression and handover

- Driver: `fx1_isp_dma_reset` (U6-A1).
- Platform tests:
  - new `fx1_smoke_isp_mas0710` (and its `_q10us` variant), added to
    `platforms/fx1_soc/ci/required_tests.txt`;
  - `fx1_smoke_isp_recovery` now checks tags and has phase E (DMA reset).
- Fixture: `gen_fixture.py` adds the AWB ownership script and the 648×368
  Resizer case. The existing tables are unchanged.
- The SDK package is regenerated from this model: sample profiles are
  recomputed by `make_profile.py`, and golden NV12 hashes come from the
  stand-alone model. The release export (`export_to_sdk.sh --replace`)
  needs a committed CDC-VP tree.
- Owner questions are batched (decisions log §9.9).

