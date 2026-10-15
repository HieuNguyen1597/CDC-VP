# DDR3 Controller — Open Questions for the HAS/MAS Owners

From: DDR3 VP team. Baseline: HAS V1.0 (sha256 `9ff25e19…`), MAS V0.1 (sha256 `64a85550…`, content
ends at §6.2.20; chapters 7–9 including the register chapter are missing).

The VP implements HAS first and MAS second. The items below cannot be resolved that way
because neither document defines them. Each item states what is missing, where we looked, what
firmware or verification would observe, and our proposed answer.

- **Part A** items have no definition anywhere. The VP implements the proposed answer as a
  PROVISIONAL encoding (DEC-13) so firmware development can proceed; please confirm or replace
  it. All of them are in one header, `include/fx1_ddr3/provisional_encodings.h`.
- **Part B** items are implemented as proposed. Please confirm or correct them.

Detailed evidence (line references) is in `docs/audit/*.md` under the GAP IDs.

## Part A — Undefined field encodings (implemented provisionally)

| ID | Register / field | Missing definition | Proposed answer | Audit ref |
|---|---|---|---|---|
| SOQ-01 | INIT_STATUS[10:7] stage | Code for each initialization stage. HAS Table 9-9 lists stage groups only. | 0 HOLD, 1 PHY_RESET, 2 PHY_READY_WAIT, 3 RESET_RELEASE_WAIT, 4 CKE_LOW_WAIT, 5 CKE_HIGH_WAIT, 6 MR2, 7 MR3, 8 MR1, 9 MR0, 0xA DLL_WAIT, 0xB ZQCL, 0xC ZQ_WAIT, 0xD TRAINING, 0xE DONE, 0xF FAILED | GAP-LC-01 |
| SOQ-02 | INIT_ERROR[15:0] | Meaning of each one-hot bit | Bit n = stage code n active when the failure occurred | GAP-LC-02 |
| SOQ-03 | TRAIN_CMD[3:1] primitive, [11:4] rank/lane, [27:12] options; TRAIN_STATUS[2:0] bit order and [15:8] terminal codes | All encodings | Primitives 0 LOAD_RDLVL, 1 LOAD_GATE, 2 LOAD_WRLVL, 3 LOAD_ALL, 4–7 reserved; [4] rank, [8:5] byte-lane mask; options opaque to the PHY. TRAIN_STATUS [0] busy, [1] done, [2] error; [15:8] 0 OK, 1 timeout, 2 stale, 3 PHY fail, 4 precondition rejected | GAP-LC-11 |
| SOQ-04 | POWER_ERROR[23:0] | Error codes | Bitmap: [0] denied busy, [1] denied illegal transition, [2] pre-cutoff timeout, [3] post-cutoff timeout, [4] wake with PHY not ready, [5] denied by commit block | GAP-LC-19 |
| SOQ-05 | PHY_BRIDGE_ERROR[7:0] | Error codes | Bitmap: [0] load timeout, [1] stale epoch/tag, [2] provider failure, [3] load while PHY not ready | GAP-LC-16 |
| SOQ-06 | ECC_LAST_INFO; ECC_LAST_ADDR_LO/HI | Bit layout of type/codeword/syndrome; update policy (CE and UE or UE only; last or first); unit (granule or byte); which of the four codeword syndromes is reported | [1:0] type (1 CE, 2 UE), [3:2] codeword, [15:8] syndrome of the lowest affected codeword; updated on every event, last wins; unit = physical granule index | GAP-BE-01..03, GAP-CSR-17 |
| SOQ-07 | QUARANTINE_FIRST_REQ_LO[26:24] token-kind codes; QUARANTINE_STATUS reset-required bit and [31:16] | MAS Tables 6-135/6-137 define the reasons (0 timeout, 1 malformed, 2 tokenless/unexpected, 3 context/kind, 4 generation) and the record layout, but not the kind codes or a reset-required bit. (Corrected in subtask 3: the earlier proposal for reasons 3 and 4 contradicted MAS Table 6-135.) | Kinds: 0 RD_DATA (no ECC check), 1 RD_ECC, 2 RMW internal read; reset-required = active; HAS-only empty_token → reason 2, phy_error → reason 1 | GAP-CSR-12, GAP-BE-12/14/15 |
| SOQ-08 | FATAL_STATUS[2:0] and bits above 7 | MAS defines [7:3] only. RMW post-write fatal, scrub fatal and stale lock release have no bit | [0] ECC fatal hold, [1] scrub fatal, [2] request sequence fault | GAP-CSR-16, GAP-BE-11 |
| SOQ-09 | Commit blocker codes ([15:8] / [23:16] of *_COMMIT_STATUS, FE_REGION_STATUS, FE_PORT_CMD_STATUS) | Only 0x04 (timing), 0x11 (topology), 0x0B–0x0D (region) are known; codes for not-quiesced, mutex busy, not idle, recovery required, DFI rejection are missing. Is an acknowledged-completion failure also SLVERR? | VP-local codes 0xE0–0xEF; SLVERR on any failed commit | GAP-CSR-05/06, GAP-BE-08, GAP-FE-10 |
| SOQ-10 | FE_PORT_CMD op 0, 2, 4–7; behaviour of soft reset without quiesce ack | MAS defines op 1 quiesce and op 3 soft reset only; no release-quiesce op | 2 = release quiesce; 0 and 4–7 reserved (SLVERR); soft reset without ack is rejected immediately (SLVERR) instead of stalling the write response | GAP-FE-13, GAP-CSR-11 |
| SOQ-11 | REFRESH_RANK0/1[6:5] urgency; REFRESH_STATUS[4:0] bit order | HAS names three urgencies in two bits, BE04 defines four; status bit order not given | 0 background, 1 normal, 2 high, 3 mandatory; [1:0] per-rank mandatory, [2] any high/mandatory, [3] any mandatory, [4] saturation | GAP-LC-17 |
| SOQ-12 | PHY_CAP / PHY_STATUS | Layout ("provider-defined") | PHY_STATUS [0] reset_done, [1] ready, [2] failure, [3] timing_wait_ok, [5:4] update observation; PHY_CAP = provider constant, 0 when unavailable | GAP-CSR-18 |
| SOQ-14 | P-channel PSTATE[1:0] and POWER_STATUS[1:0] resident-state codes; TRAIN_OBSERVATION0 packing of the BE09 observation | HAS names the four power-state classes and the observation fields but no encodings | PSTATE 0 ACTIVE, 1 APD, 2 PPD, 3 SR; TRAIN_OBSERVATION0 [7:0] read-level, [15:8] write-level, [17:16]/[19:18]/[21:20] modes, [24] rank | (subtask 2) |
| SOQ-13 | QoS class encoding of the sideband table (Table 1-4, 2-bit class) | Only NORMAL = 1 can be inferred (MAS QOS_LUT reset 0x55555555) | 0 BULK/BE, 1 NORMAL, 2 LATENCY/SRT, 3 HRT | GAP-FE-07 |
| SOQ-36 | SERVICE_COUNT_BE / _SRT / _HRT class membership | Three backend service classes, four 2-bit frontend codes; no mapping in either document | BE counts codes 0 and 1, SRT code 2, HRT code 3; one count per accepted backend atom, free-running 32-bit | (subtask 3) |

## Part B — Implemented as proposed; please confirm

| ID | Topic | VP behaviour | Audit ref |
|---|---|---|---|
| SOQ-20 | Region address normalization | The backend sees only (A − base)/32. Every region of every port therefore maps from DRAM user offset 0 and regions alias each other. Is this intended, or should a region carry a DRAM offset? | GAP-FE-01, C3 |
| SOQ-21 | Legality basis with a non-4-KiB-aligned region base | 4 KiB, WRAP alignment and container checks use the global AXI address | GAP-FE-02, C17 |
| SOQ-22 | Exclusive access spanning more than one 32-byte granule | Read creates no reservation (OKAY); write fails with OKAY and no update | GAP-FE-03 |
| SOQ-23 | RRESP across beats after a granule error | Sticky forward: the error beat and all later beats carry at least that error | GAP-FE-14, C5 |
| SOQ-24 | INIT_* wait units | Core (controller) cycles, as HAS §9.1.1 states; MAS says tCK | C-08 (lifecycle audit), GAP-LC-04 |
| SOQ-25 | Uncorrectable old data during RMW | BE02: SLVERR, no write, lock released; fatal only after the first physical write (HAS §6.2.19 body says fatal) | C-08 (backend), GAP-BE-18 |
| SOQ-26 | Physical capacity above 2^27 granules | Accepted, usable capacity clamped to 2^27 (HAS); MAS rejects it | C-05, GAP-BE-21 |
| SOQ-27 | Initialization lock for INIT_RW words | Locked from leaving HOLD until DONE/FAILED and during runtime training; writes then return SLVERR | GAP-LC-06 |
| SOQ-28 | Quiesce prerequisite for boot-time commits | Satisfied when mission admission has never opened since reset and the controller is idle | GAP-LC-07 |
| SOQ-29 | Snapshot composition | All snapshot-backed registers are captured atomically under one sequence; the 30-word packing is not modelled | GAP-CSR-20 |
| SOQ-30 | IRQ_RAW[4], [5], [9], [10] triggers | [4] commit validation reject or region decode error, [5] refresh saturation, [9] PHY failure rising edge, [10] mutex busy / endpoint failure / commit reject | GAP-CSR-10, GAP-CSR-04 |
| SOQ-31 | TOPO_SHADOW[12] map profile and [10:9] page code | [12]: 0 linear, 1 interleaved (MAS mask bit order). [10:9]: code = log2(words/256), confirmed by MAS reset examples 0x501 / 0x6F1 | GAP-BE-06/07 |
| SOQ-32 | CE_THRESHOLD = 0 | Rejected with SLVERR (HAS "nonzero"); MAS says 0 disables the interrupt | C-16 (backend), GAP-BE-16 |
| SOQ-33 | T_CKSRE/T_CKSRX at 0x30CC/0x30D0 reset to 1, below the JEDEC floor of 5 | Stored, read back and used as programmed (HAS); the MAS floor is not enforced. The programming guide tells firmware to write at least the JEDEC value | GAP-BE-20 |
| SOQ-34 | Duplicate decision ID BE13 (timing conversion and serializer) | Cited as BE13(timing) and BE13(serializer) | GAP-BE-24 |
| SOQ-35 | Scope of "failure" that clears init_complete | HAS Table 1-4 says *initialization* failure clears it; BE08 / §9.1.1 say "reset, reinitialization or failure". The VP clears it on reset, reinitialization and initialization failure, and keeps it high after a later runtime PHY or training failure (mission readiness closes instead) | HAS Table 1-4 vs BE08 |
| SOQ-37 | ECC writes during a read quarantine | MAS Table 6-136 lets "writes needing no old-data read" continue, but every ECC write reads its companion granule to keep the sibling records. The VP therefore answers every ECC write with SLVERR and no write while quarantined; writes without ECC continue. Should a full ECC write continue instead (record write with byte enables, no companion read)? | VPL-25 |
| SOQ-38 | Reset domain of CE_COUNT, UE_COUNT, ECC_LAST_*, SERVICE_COUNT_* | "Cleared only by reset" without naming the domain. The VP clears them on core (and cold) reset, not on CSR reset, because the ECC engine and scheduler are core-side | VPL-24 |
