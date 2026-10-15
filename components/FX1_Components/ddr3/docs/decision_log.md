# DDR3 Controller VP — Decision and Issue Log

Rules: HAS wins over MAS. If HAS is silent and MAS defines the behaviour, MAS is adopted
and marked. If neither defines a behaviour that firmware or tests can observe, the item
is logged as an open issue (`GAP-*`) with a proposed VP default. That default may be
implemented only when the item does not change the CSR ABI. Otherwise the user decides.

Detailed per-section audits are in `docs/audit/`. This log keeps the decisions that
change implementation.

## 1. Specification decisions (from the plan, §3)

| ID | Topic | Decision |
|---|---|---|
| SPEC-01 | HAS is a review draft with Proposed defaults | Implement the documented rules as the VP baseline; keep decision IDs in traceability; never call them silicon-confirmed |
| SPEC-02 | MAS chapters 7–9 referenced but missing | Use HAS §7–9; never infer from missing MAS chapters |
| SPEC-03 | Timing shadow reset: HAS = 0, MAS = speed-bin image | CSR reset/readback follows HAS (0). MAS speed-bin values are used by tests, preinit mode and FW examples to write a legal image before commit |
| SPEC-04 | QoS: HAS sideband (Table 1-4) vs MAS CSR | Sideband per HAS. No invented CSR. Admission stays blocked until the QoS table is configured (FE05). Platform provisions it (DDR_SYSCTRL, DEC-04) |
| SPEC-05 | Timing image: HAS 19 words, MAS 31 + 12 DFI words | Public ABI = HAS. Extra MAS words become build parameters (DEC-08) |
| SPEC-06 | 820 MHz target vs speed bins | VP baseline DDR3-1600, CK 800 MHz, controller 400 MHz at ratio 2. 820 MHz is not a VP default |
| SPEC-07 | ECC usable capacity | U = min(8 × floor(P/9), 2^27) granules, exactly |
| SPEC-08 | Startup scrub zeroes data + ECC | Never preload DDR before scrub; preload only after scrub or from a non-DDR source |

## 2. User decisions (plan §3.1)

| ID | Decision |
|---|---|
| DEC-01 | Review after each subtask; work on `dev`, no commits |
| DEC-02 | Approx-timed data path: per-rank bank/open-row state and DRAM timing set per-granule latency; no cycle-level OoO scheduler |
| DEC-03 | Microarchitecture status words without bit layout read 0 (unavailable); SERVICE_COUNT_* counted |
| DEC-04 | SoC glue block DDR_SYSCTRL at 0x12010000 (64 KiB, SysBus0) mirrors IP output levels, drives P-channel and QoS/reserved-rate sideband; no fault injection there |
| DEC-05 | INIT_CMD → SLVERR. Init starts at cold reset in hold; it advances only when TOPO, TIMING and DFI active images are all valid; each later successful commit of those groups restarts full init (BE05) |
| DEC-06 | Boot training is automatic in the init training-handoff stage, using the INIT_RW PHY delay words, per active rank, tag 0. TRAIN_CMD is runtime training only |
| DEC-07 | Memory ports receive global AXI addresses (bus translation off for MEMCTL_DDR). Refined after the P0 audit (HAS §6.1.15 T6-29, Table 8-5): until the first successful FE_REGION_COMMIT, address decode uses the bootstrap map sideband while FE_REGION_ACTIVE reads 0 and FE_REGION_STATUS[16] reads 0; after a commit decode uses the active image; core offline reverts decode to the bootstrap map |
| DEC-08 | MAS-only CSRs without offsets are not exposed; their values are build parameters from MAS Tables 5-15..5-18; periodic ZQ is internal; frequency change and ZQ_CMD are out of scope |
| DEC-09 | fx1_soc default boot mode `preinit`; new firmware-init tests and a FreeRTOS firmware-init test are added |
| DEC-10 | Keep 4 GiB physical baseline; scrub time is computed analytically; firmware-init tests get a larger timeout |
| DEC-11 | Deliverable docs and code comments in English |
| DEC-12 | Register field encodings that neither HAS nor MAS defines are not implemented until the spec owner answers. They are listed as Part A of [spec_owner_questions.md](spec_owner_questions.md); dependent behaviour is marked BLOCKED-SPEC in the traceability matrix. Part B items are implemented as proposed and await confirmation |
| DEC-13 | Supersedes the blocking part of DEC-12 (user, before subtask 2): the Part A proposals of [spec_owner_questions.md](spec_owner_questions.md) are implemented as PROVISIONAL encodings, all defined in `include/fx1_ddr3/provisional_encodings.h` with their SOQ IDs, marked in the C header, the programming guide and the traceability matrix. They will be replaced when the spec is completed; the questions to the spec owner stay open |

## 3. HAS vs MAS conflicts resolved during P0

| ID | Item | HAS | MAS | Resolution |
|---|---|---|---|---|
| CONF-01 | CAS latency range | 5–16 "where device and profile permit" (Table 2-6 req 10, Table 5-1) | 4-bit register, 5–11 supported, 12–16 reserved and rejected (Table 5-3, 5-20) | Consistent: MAS states the profile limit HAS defers to. TIMING_COMMIT rejects CL outside 5–11 |
| CONF-02 | T_CKSRE / T_CKSRX at 0x30CC / 0x30D0 | INIT_RW, reset 1 (Table 8-5) | Reserved, read 0, ignore writes; values live in timing bank (§5.6) | HAS wins: INIT_RW storage with readback. The internal power sequence uses these INIT_RW values |
| CONF-03 | TOPO_SHADOW reset | 0x00000000 | 0x00000501 | HAS wins (SPEC-03) |
| CONF-04 | Exclusive reservation granule | 32-byte granule + same byte enables (§6.1.12) | 128-byte window, 4 entries (Table 1-4, 5-6) | HAS wins: 32-byte granule match. Entry count 4 adopted from MAS (HAS silent) |
| CONF-05 | Live PHY update (dfi_phyupd) | Excluded (BE10, Table 3-1) | Supported, answered within 25 DFI clocks (Table 1-4) | HAS wins: excluded. PHY update observations captured only |
| CONF-06 | Frequency change | Not in HAS profile | Supported via FREQ_CHANGE_CMD (§5.2) | Out of scope (DEC-08) |
| CONF-07 | Configuration sideband | Bootstrap map + QoS sideband (Table 1-3, 1-4) | "No sideband configuration port" | HAS wins (SPEC-04, DEC-07) |
| CONF-08 | Timing reset images legal at reset | RS11: timing_valid low until a complete image passes checks | Reset images legal, commit accepted with no write | HAS reset is 0, so a commit without writes fails validation in the VP |

## 4. Conflicts found by the P0 audits

The audits list every conflict with line references:
[frontend C1–C17](audit/frontend_requirements.md#3-has-vs-mas-conflicts),
[backend C-01–C-20](audit/backend_requirements.md#4-has-vs-mas-conflicts),
[interfaces/lifecycle C-01–C-32](audit/interfaces_lifecycle_requirements.md#4-has-vs-mas-conflicts).
All were resolved by the precedence rule (HAS wins; HAS silent and MAS defines → MAS adopted and
marked). Resolutions that change implementation beyond §3:

| Ref | Resolution |
|---|---|
| FE C4 / DEC-07 | Bootstrap map is the effective decode map until the first region commit |
| FE C8, C16 / LC C-19 | Region commit: same-port overlap only; BASE and SIZE multiples of 32; region end within USABLE_GRANULES; failure answers SLVERR with blockers 0x0B–0x0D (MAS) |
| FE C12 | EXOKAY on an exclusive read only when a reservation is created |
| FE C13, C14, C15 | Exclusive byte coverage from address/SIZE/LEN (MAS); port soft reset resets the QoS table (MAS); FE_PORT_CMD op 1 = quiesce, op 3 = soft reset (MAS) |
| FE C6 / LC C-22 | No port quiesce timeout; QUIESCE_STATUS[6] and FE_PORT_STATE timeout read 0 |
| BE C-05 | Physical capacity up to 2^28 granules accepted; usable capacity clamped to 2^27 |
| BE C-08 | Uncorrectable old data during RMW follows BE02 (SLVERR, no write); fatal only after the first physical write |
| BE C-09 | Granule lock held until data and record writes complete |
| BE C-13, C-14 | Scrub mapping fault is sticky fatal; prerequisite loss returns to WAIT and restarts at granule 0 |
| BE C-16 / LC C-09 | CE_THRESHOLD = 0 is rejected (SLVERR) |
| BE C-17 / LC C-23 | MAS-only registers (ERROR_STATUS, BAD_RETURN_LAST, WRITE_FETCH_TIMEOUT, SCHED_EVENT_COUNT, SCHED_DRAIN_STATUS, FE_QUIESCE_TIMEOUT, extra DFI words) are not CSR-visible (DECERR); they extend the DEC-08 list |
| BE C-18, C-19, C-20 | SECDED error classes, READ_TIMEOUT lower bound and full-write companion read follow MAS |
| LC C-08 | INIT_* waits count controller cycles (HAS); confirmation requested (SOQ-24) |
| LC C-16 / GAP-LC-21 | Every power entry stops admission and drains (HAS); mission traffic waits for wake |
| LC C-17, C-18, C-24, C-25, C-31, C-32 | MAS refinements adopted: extra mission_ready terms, GLOBAL_CTRL.enable as an admission term, FATAL_STATUS[7:3], QUARANTINE_STATUS [1:0]/[4:2]/[15:8] with reason 0 = timeout, single-bit INIT error forwarding, MR write validation |

## 5. Open issues

Every gap the audits found is listed with its proposal in the audit files
(GAP-FE-01..24, GAP-BE-01..28, GAP-LC-01..23, GAP-CSR-01..21). Handling:

| Class | Rule | Items |
|---|---|---|
| PROVISIONAL | Field encodings undefined by HAS and MAS. Implemented with the SOQ Part A proposal (DEC-13), replaceable in one header | SOQ-01..14 and SOQ-36 in [spec_owner_questions.md](spec_owner_questions.md) |
| CONFIRM | Behaviour implemented as proposed; spec owner asked to confirm | SOQ-20..35, SOQ-37..38 |
| VP-LOCAL | TLM/abstraction choices with no hardware ABI (blocking stall while admission is closed, admission instant, acceptance order, data layout, debug transport, out-of-range extension values, reset of in-flight calls) | GAP-FE-04/05/06/15/16/17/18/19/20/21/22/23, GAP-BE-26 |
| ADOPTED | Remaining proposals in the audits that change no FW-visible encoding | all other GAP items |
| GAP-00 | CAP_WIDTH0[23:19] reports the fixed bundle count 4, not DDR_SYS_N_PORT | ADOPTED (HAS literal) |

## 6. Subtask 1 review follow-up (2026-10-14)

Reviews: `plan/DDR3_VP_SUBTASK1_REVIEW.md` (five findings) and `plan/DDR3_VP_SUBTASK1_REVIEW_R2.md` (one follow-up). All accepted.

| Finding | Resolution |
|---|---|
| R1 DFI delay underflow | Derived DFI delays are computed in signed 64-bit and must lie in 0..255 (MAS Table 5-20); `validate()` rejects builds that violate it (field `phy_latency`), `dfi_image()` throws instead of wrapping, T_RTRS < 2^31 is checked. Boundary tests per speed bin (`core dfi_bounds`) |
| R2 CSR buffer handling | Command, data pointer and byte-enable length are checked before any buffer access; only reads write the buffer. Tests `tlm csr_robustness` |
| R3 bootstrap wording | Inventory and header now separate the effective decode map (bootstrap) from the committed CSR image (reads 0 until the first commit) |
| R4 blocked encodings in code | PHY_CAP/PHY_STATUS are opaque provider words (`provider_cap_word/status_word`, no VP layout); QoS class is a plain 2-bit code with only NORMAL = 1 named |
| R2-1 stop requests ignored by admission | Core reset request and port quiesce request added to the admission gate; request changes wake blocked callers. Traffic tests release stop conditions from a separate actor thread (`tlm reset_contract` T1–T7) |
| R5 reset/quiesce contract | Reset/quiesce bundle API added (interfaces.md §5): cold/core/CSR/port resets, core/CSR reset request → safe, per-port quiesce request/ack, soft reset pending/hold. Test `tlm reset_contract` |

Decisions taken while fixing (VP-local, no CSR ABI impact):

| ID | Decision |
|---|---|
| VPL-01 | Admission stop conditions — physical core reset, core reset request, port quiesce request, port soft-reset hold — stall memory requests until the last one is released (HAS §7.1, §6.1.14, §8.3; FE-6.1.14-01/05, LC-7.1-02; corrected after review R2). Only an access on a port whose own AXI domain (or the CSR domain) is in reset is an integration error |
| VPL-02 | Malformed CSR payloads (non read/write command, null data, byte-enable length ≠ 4) get TLM error statuses, not AXI4-Lite responses |
| VPL-03 | A reset never erases DRAM contents; retention is not promised to firmware (LC-7.1-07) |
| VPL-04 | Test harness: a SystemC case passes only if its scenario signals completion, so a hung thread fails |

## 7. Subtask 2 decisions (CSR, configuration, lifecycle)

| ID | Decision | Basis |
|---|---|---|
| S2-01 | DEC-13 implemented: every provisional encoding lives in `include/fx1_ddr3/provisional_encodings.h` with its SOQ ID; it also defines PSTATE / POWER_STATUS[1:0] codes (0 ACTIVE, 1 APD, 2 PPD, 3 SR), TRAIN_OBSERVATION0 packing and blocker 0xE2 (cancelled) | DEC-13 |
| S2-02 | Build validation applies the full DFI_TIMING_COMMIT legality (formulas, >= R, < 256, calendar bound with CAL_DEPTH 32, k_wr 2, k_rd 4, tphy_wrdata 1). Consequence: a native (R = 2) DDR3-800 build with the default PHY write latency 2 is rejected (tphy_wrlat = 1 < R); set PWL <= 1 for that bin. This tightens the subtask 1 rule | MAS Table 5-19 rule 6, MAS §6.2.12 |
| S2-03 | DFI_TIMING_COMMIT requires the DFI words to equal the formulas against the active timing image; a later TIMING_COMMIT that breaks the equality invalidates the DFI image, so init waits in HOLD until a matching DFI commit | MAS §6.2.12 "checks both formulas" |
| VPL-05 | Each AXI4-Lite access returns one controller cycle after it is decided. "Same cycle" for hardware-set-wins is the same simulated time; with the access latency, successive accesses of one master never share a cycle | HAS §8.5; VP-local |
| VPL-06 | Commits, quiesce, FE_PORT_CMD and snapshots complete after a 4-controller-cycle endpoint latency (plus the drain or hold they wait for) | VP-local (CDC abstracted) |
| VPL-07 | Core reset invalidates the installed topology/timing/DFI/region images (destination lost); the CSR shadows survive and firmware commits again. CSR reset returns every CSR-side word to reset but leaves the core images installed | CSR-8.1-22, LC-7.1-05; VP-local for the CSR side |
| VPL-08 | One SC_METHOD drives the output ports (irq, mission_ready, init_complete, idle, drained); a state change shows on the port one delta later. Getters return the state immediately | SystemC single-writer rule |
| VPL-09 | Subtask 2 frontend admission gate: reset, reset request, port quiesce (sideband or FE_PORT_CMD), soft-reset hold, QoS configured. Superseded by VPL-18 (subtask 3) | GAP-FE-22 |
| VPL-10 | PHY observations from an older epoch are discarded with PHY_BRIDGE_ERROR stale; a current-epoch identity mismatch (tag or rank) fails the episode | HAS §8.3, BE09 |
| VPL-11 | Runtime TRAIN_CMD is rejected (code 4) unless initialization is done, the power state is ACTIVE, no training/PHY failure or recovery is pending and the rank is active; runtime tags cycle 1..15 | GAP-LC-12 |
| VPL-12 | In APD/PPD refresh deadlines are missed until a wake (debt grows, PACTIVE rises at high urgency); self-refresh holds the debt and restarts the interval on exit | HAS §9.2, GAP-LC-22 |
| VPL-13 | P-channel entry is denied (busy) while initialization is not done, scrub or training runs, recovery is required or the command mutex is held | LC-9.3-01, GAP-LC-23 |
| VPL-14 | Startup scrub duration = (U + U/8) x 4 tCK (one BL8 write per granule), restarted from granule 0 on prerequisite loss | BE03, DEC-10 |
| VPL-15 | Host fault-injection flags (e.g. refresh block) survive reinitialization | VP-local |

## 8. Subtask 2 review follow-up (2026-10-14)

Review: `plan/DDR3_VP_SUBTASK2_REVIEW.md`. All five findings accepted; root cause: accepted
commands had no uniform ownership and termination across resets and PHY failure.

| Finding | Resolution |
|---|---|
| S2-R1 CSR reset during runtime training | Each training episode has an identity. A CSR reset retires the command's core-side episode (no callback), clears busy/runtime flags, marks the rank unavailable (training error) and reports TRAIN_STATUS error with provisional code 5 CANCELLED. A core reset retires every episode in `init_restart()` and the waiting command ends with SLVERR, freeing its own mutex token |
| S2-R2 core reset during a power transition | The mutex has an owner (CSR command or power transition) and a token; only the owner or the reset of its own domain frees it. Core reset retires the power transaction, frees its token and answers the pending P-channel request with PDENY. A CSR reset leaves a power-owned mutex held |
| S2-R3 PHY failure during training | A rising PHY failure terminates the active episode (boot or runtime) with PHY_FAIL independently of ready; a failed init attempt retires its training work; the boot result applies only to the attempt that started it |
| S2-R4 FE_PORT soft reset cancelled by core reset | Commands capture CSR/core epochs at acceptance (`CmdGuard`) and re-check them after every wait; the soft-reset wait completes only on a counted hold completion; a core, CSR or port reset ends it with SLVERR and blocker 0xE2 (cancelled) |
| S2-R5 TRAIN_CMD lane mask and options | `PhyLoadRequest` carries `lane_mask` (TRAIN_CMD[8:5]) and `options` (TRAIN_CMD[27:12]) to the provider; boot training uses all lanes; a command selecting no lane is rejected (code 4) |
| (self) snapshot cancelled by CSR reset | A capture cancelled by a CSR reset no longer clears the busy flag of a newer capture |

| ID | Decision |
|---|---|
| VPL-16 | Every accepted command has exactly one terminal outcome: success, rejection, or cancellation by a reset of its CSR, core or port domain. After a CSR reset the command writes no CSR-side state; after a core or port reset it reports the cancellation and frees its own mutex token |
| VPL-17 | A CSR reset cancelling runtime training leaves the rank unavailable (training error) because the PHY delay state is unknown; firmware recovers with quiesce + full reprogramming (the CSR reset cleared the shadows) or with a reset |

Second review (`plan/DDR3_VP_SUBTASK2_REVIEW_R2.md`): S2-R1, R2, R3, R5 closed. S2-R4a (a port
reset pulse released within the FE_PORT endpoint wait was missed for QUIESCE/RELEASE) fixed with a
port-reset generation counter (`port_reset_gen_`, counted on every port reset assertion including
cold reset), captured at acceptance and re-checked after every wait; it is separate from the
scheduler epoch that a soft-reset hold advances. init_complete after a runtime failure: kept high,
asked as SOQ-35.

## 9. Subtask 3 decisions (data path, ECC, error handling)

| ID | Decision | Basis |
|---|---|---|
| VPL-18 | Two admission levels. The port (frontend) gate holds new transactions on: physical core reset, core reset request, port quiesce (sideband or FE_PORT_CMD), region-publication quiesce, soft-reset hold, QoS not configured, QUIESCE_CTRL request, GLOBAL_CTRL admission stop, and all N_TAG (32) contexts of the port in use. The backend gate holds accepted legal transactions on GLOBAL_CTRL.enable = 0 or mission readiness low (init, training, scrub, power state, fatal, recovery). No stop fails an accepted transaction; decode errors complete locally without the backend | MAS Table 6-73, HAS §6.1.1/§6.1.14/§7.1, C-18, GAP-FE-04/22 |
| VPL-19 | One `b_transport` = one AXI transaction. Order: same-ID chain per port across both directions (acceptance order), R data per port in AR acceptance order across IDs, no cross-port order. Errored transactions keep their position | HAS §6.1.5, §6.1.8, GAP-FE-05/06 |
| VPL-20 | Latency: decode errors and quarantine/exclusive-miss local completions add none; backend work = in-order per-bank model (`BankTimer`: open row per (rank, bank), tRCD/tRP/tRAS/tRRD/tFAW/tCCD/CL/CWL/AL/tWTR/tRTW/tRTRS, BL8) plus 4 controller cycles on the response path. Refresh follows the lifecycle grid: each REF window (origin + k·tREFI, tRFC long) closes the rank's rows and delays its commands; debt repayment after a block keeps the rank busy; a blocked or held rank has no window. PPD/SR entry closes rows. Companion accesses are timed too and may share the data bank, so ECC throughput is pessimistic. Performance is not claimed | DEC-02, MAS Table 6-98/6-99 |
| VPL-21 | Storage is keyed by physical granule index (data granule d, companion granule e); DRAM coordinates drive timing only, so a geometry change does not scramble data (BE-6.2.2-19). `GranuleStore` reimplements `fx1::SparseStorage` semantics (4 KiB pages on first nonzero write, absent bytes read 0) with range erase, so the package stays standalone | plan §4.1/§4.4 |
| VPL-22 | Hazard order: every request queues on the granules it touches (FIFO by backend install); it waits to head every queue. Reads take their data when it returns; writes without ECC apply at service start; ECC writes apply after their internal reads (companion, plus old data when partial). The lock is held until the write data is on the bus (C-09 HAS wins) | HAS §6.2.5, §6.2.18, BE02 |
| VPL-23 | ECC write rules: a full write (32 strobes) reads the companion only and never decodes; a partial write decodes the old data, aborts with SLVERR and no write on a UE in a lane it does not fully overwrite, and heals and counts a UE in a fully overwritten lane; the record is written with 4 byte enables (slot only). One event per atom | BE02, MAS ECC-CT-01/02, GAP-BE-17 |
| VPL-24 | ECC reporting: CE_COUNT += corrected lanes (saturating), UE_COUNT += 1 per UE event; IRQ_RAW[0] once when old < CE_THRESHOLD <= new, IRQ_RAW[1] per UE; ECC_LAST_ADDR = physical data granule; ECC_LAST_INFO per SOQ-06 with [15:8] = {mismatch, syndrome[6:0]} of the lowest affected lane; last event wins. The counters, ECC_LAST_* and SERVICE_COUNT_* are core-domain state: cleared by core and cold reset, not by CSR reset | MAS Table 6-156, GAP-BE-01..03/16 |
| VPL-25 | Read quarantine (MAS §6.2.11): reasons and the first-cause record follow MAS Tables 6-135/6-137 (this replaces the SOQ-07 reason proposal); token kind codes are provisional. READ_TIMEOUT counts tCK from service start. Returns arriving no later than the set point complete; later ones are discarded. While active: new reads SLVERR with zero data, ECC writes SLVERR with no write (they need the companion read), writes without ECC continue. Sticky until cold reset (RS10) | MAS §6.2.11, GAP-BE-12/13/15 |
| VPL-26 | Read-return faults happen only by injection: the modelled return delay is always below the legal READ_TIMEOUT floor | GAP-BE-26 |
| VPL-27 | Fatal containment: FATAL_STATUS bits are sticky until cold reset and close readiness; work that has not launched is held (VPL-41). A write-result error after an ECC write launched sets FATAL_STATUS[0] and keeps the granule lock (ECC_CAP_STATUS[8:2]); without ECC it is SLVERR only. Issued writes are never rolled back | HAS §6.2.10, BE02 |
| VPL-28 | Startup scrub data effect is a frontier: the zero writes passed so far are applied to the store when it is accessed, by dropping resident pages (pages left all zero are released); no page is allocated. A restart begins at granule 0; writes accepted before an abort stay applied. A preload before the frontier passes is erased, with a warning | BE03, RS05, DEC-10, SPEC-08 |
| VPL-29 | Response construction: worst status (DECERR > SLVERR > EXOKAY > OKAY); per-beat RRESP sticky forward; an error beat returns zero data; EXOKAY only for an exclusive access that met its reservation and had no error | HAS §6.1.13, SOQ-23, C12 |
| VPL-30 | Exclusive monitor: HAS 32-byte granule of the normalized address (reservations alias across regions like the data); MAS allocation (own entry, lowest free, round-robin victim); a single-granule access only; coverage from address, SIZE and LEN; a taken write with at least one strobe clears the granule; a fully masked write (exclusive or not), decode errors and debug writes do not; port quiesce, publication quiesce, an acknowledged power stop (VPL-43), port reset and core reset invalidate. Every monitor decision (exclusive-write verdict, ordinary-write invalidation, exclusive-read reservation) is taken at the backend take boundary, in install (granule-lock ticket) order, which is also the order of the data effects; an exclusive-write miss completes locally with OKAY (S3-R3a) | HAS §6.1.12, C1/C2/C12/C13, GAP-FE-03/24 |
| VPL-31 | Power entry and runtime training wait for the backend to drain (accepted transactions wait at the backend gate). The drain counts toward POWER_TIMEOUT: a drain longer than it is denied before the cutoff | LC-9.1.2-08, LC-9.3-01 |
| VPL-32 | FE_REGION_COMMIT raises a publication quiesce on every port, waits for every enabled port to drain (indefinitely), installs atomically, then releases | HAS §6.1.15, GAP-FE-12 |
| VPL-33 | Accepted transactions aborted by a core reset or a reset of their port return SLVERR with a warning (no response is promised, GAP-FE-21); data effects already applied remain. A blocked, not yet accepted call simply waits | GAP-FE-21 |
| VPL-34 | Memory payloads that disagree with the AXI contract are model-usage errors with TLM statuses: command/address/ID width -> COMMAND_ERROR, null data -> GENERIC_ERROR, length != (LEN+1) x bus bytes -> BURST_ERROR, byte-enable length != data length -> BYTE_ENABLE_ERROR | GAP-FE-18 |
| VPL-35 | IRQ_RAW[4] rises on a region decode miss (SOQ-30) and on a backend geometry/range error event (GAP-BE-28); an out-of-capacity DECERR is poison without an event | SOQ-30, HAS Table 6-33 |
| VPL-36 | Platform bridge (`GenericAxiBridge`): plan_generic_access split, ID 0, constructor QoS, AxLOCK 0, address offset for local/global translation, generic byte enables honoured, every burst issued even after an error (no rollback), worst response, no DMI, transport_dbg forwarded | plan §4.2, GAP-FE-16 |
| VPL-37 | Host debug/preload API addresses user (normalized) offsets of the committed geometry, regenerates the companion record on writes, returns raw stored bytes on reads, raises no interrupt and leaves admission, order and reservations alone; `transport_dbg` on a port uses the effective region map and requires the whole range inside one region | GAP-FE-17, plan §4.4 |
| VPL-38 | QUIESCE_STATUS: [1] stop acknowledged = (request or admission stop) and frontend drained; [2] global idle (no frontend or backend work); [3]/[4] backend idle; [5] complete = request and global idle | MAS Table 6-73 "stop acknowledge" |
| VPL-39 | ECC_CAP_STATUS[1] (an ECC write parent in service) and [8:2] (granules with queued owners plus fatal-held locks, APPROX occupancy) are captured by SNAPSHOT_CMD with the other snapshot words and read 0 before the first capture; [0], [12:9], [20:13] are constants | HAS Table 8-16 ("snapshot", "coherent") |
| VPL-40 | SERVICE_COUNT_BE/SRT/HRT count backend atoms by the class captured at acceptance: classes 0 and 1 -> BE, 2 -> SRT, 3 -> HRT (provisional, SOQ-36); free-running 32-bit | HAS §8 address summary (SERVICE_COUNT), DEC-03 |

Correction: SOQ-07 had proposed reason codes 3 = stale epoch and 4 = tokenless. MAS Table
6-135 defines them (3 = context/kind mismatch, 4 = generation mismatch, 2 = tokenless or
unexpected); the register header now carries the MAS values and only the token-kind codes
remain provisional.

## 10. Subtask 3 review follow-up (2026-10-15)

Review: `plan/DDR3_VP_SUBTASK3_REVIEW.md`. All four findings accepted.

| Finding | Resolution |
|---|---|
| S3-R1 queued write launched after a fatal event | Fatal containment is re-checked at every point where unlaunched work could start: the backend gate, the request-slot wait, the granule-lock wait, the launch (bank timing) point, and for ECC writes the irreversible data/record write after the internal reads. Such work is held, never completed, until a core, port or cold reset aborts it with SLVERR. Launched work (reads in flight, issued writes) finishes |
| S3-R2 masked exclusive write consumed a reservation | A write whose strobes are all zero emits no atom: it completes locally in its same-ID turn and takes no reservation (`ExclusiveMonitor::exclusive_matches`, non-mutating): EXOKAY when it matches (GAP-FE-03), OKAY otherwise |
| S3-R3 quarantine answered frontend-only writes SLVERR | Fully masked writes complete before the backend; the exclusive-write verdict moved to backend ingress, before the quarantine/ECC handling, so a miss completes locally with OKAY. Real ECC writes in quarantine still fail with no write |
| S3-R4 reservations survived a power stop | Every accepted low-power entry acknowledges a power stop once the backend drained; it invalidates every port's reservations. A denied entry (before the drain completed) or a same-state request does not |

| ID | Decision | Basis |
|---|---|---|
| VPL-41 | Fatal containment holds unlaunched work (backend gate, slot wait, lock wait, launch point, ECC write launch), and every transaction that waits behind a contained one (same-ID chain, read-return order; S3-R1a). Held transactions are not counted against core reset-safe (the coordinated reset is the recovery path); they still count for port quiesce, fe_drained and global idle | HAS §6.2.10, MAS Table 6-73 ("contained, not completed") |
| VPL-42 | Writes with no strobed byte complete locally (OKAY, or EXOKAY for a matching exclusive) without backend admission, service counting or reservation effects | MAS §6.1.6 masked ordinals, GAP-FE-03 |
| VPL-43 | Power stop: MAS does not list which P-channel transitions raise `i_power_stop`; the VP drains the backend for every low-power entry (VPL-31), so every accepted entry (APD, PPD, SR) acknowledges a power stop and invalidates the reservations of every port (`i_port_invalidate = quiesce OR power_stop_ack`). Wake does not stop again | MAS §6.1.12, §6.1.2 |

Second review (`plan/DDR3_VP_SUBTASK3_REVIEW_R2.md`): S3-R2 and S3-R4 closed. Two follow-ups:

| Finding | Resolution |
|---|---|
| S3-R3a exclusive verdict before an in-flight ordinary write invalidated the reservation (lost update) | The verdict, the ordinary-write invalidation and the exclusive-read reservation all happen at the take boundary in `backend_service()`: after the request slot, with no wait until the granule-lock enqueue, so they follow install order; the invalidation no longer waits for the data to land. A miss still completes locally with OKAY before quarantine/ECC handling |
| S3-R1a a same-ID follower of a contained transaction blocked reset-safe | A transaction waiting in the same-ID chain or in read-return order behind a fatal-contained one is itself contained; containment transitions wake the waiters |

## 11. Subtask 4 decisions (verification and packaging)

| ID | Decision | Basis |
|---|---|---|
| VPL-44 | The controller drives the PHY `clock_disable` level: set after a completed self-refresh entry, cleared at the start of the wake and on reinitialization. Power-down keeps the clock | HAS Table 1-4, MAS (DRAM clock stops only in self-refresh) |
| VPL-45 | Requirements that the VP deliberately does not model carry the traceability status `Limitation`, each tied to an L-ID in `docs/limitations.md`; no requirement stays Planned or Partial at hand-off | Subtask 4 completion criterion |
| VPL-46 | Randomized regressions are seeded (argument or `DDR3_SEED`), print the seed and a digest of every response, and one CTest case checks that a seed reproduces its digest | Plan P6 (reproducible seeds) |
| VPL-47 | The standalone build carries a package test: install, configure and build a consumer that uses only public headers, then run it | Subtask 4 (consumer from package) |
| VPL-48 | Efficiency is measured, not thresholded: idle simulation executes no scheduled action; storage stays proportional to written data plus ECC records | Plan risk table (wall time / RSS measured on a recorded workload) |

Subtask 4 review (`plan/DDR3_VP_SUBTASK4_REVIEW.md`), two findings accepted:

| Finding | Resolution |
|---|---|
| S4-R1 installed package did not locate SystemC like the source build | One discovery module, `cmake/fx1-ddr3-find-systemc.cmake`, is used by the source build and installed with the package. It reuses an existing target, searches `SYSTEMC_HOME` (CMake variable, then environment) exclusively and first, then the default SystemCLanguage package search, then a header/library search. Five consumer tests (clean build directories) cover every documented method, including installations without CMake metadata |
| S4-R2 WriteDataProtocol and StaleLockRelease had no regression | `ecc fatal_sources` injects each immediate source from a cold-reset controller and checks the exact FATAL_STATUS bit, IRQ_RAW[11], closed admission, held traffic, persistence across a core reset and the cold-reset clear. The scrub-fatal check now requires the exact bit |

Second review (`plan/DDR3_VP_SUBTASK4_REVIEW_R2.md`): S4-R2 closed. S4-R1a: the consumer fixture
took its SystemC prefix from the producer's `SYSTEMC_HOME` input, which is empty when the
producer used `CMAKE_PREFIX_PATH`. The fixture now receives the include directory, library
file and installation root of the producer's resolved `SystemC::systemc` target, builds the
header/library-only prefix from them, and a sixth case covers explicit
`SYSTEMC_INCLUDE_DIR`/`SYSTEMC_LIBRARY`. All six cases pass for producers configured by
`SYSTEMC_HOME`, by `CMAKE_PREFIX_PATH` only, and by explicit header/library.

## 12. Subtask 5 decisions (fx1_soc integration and CPU acceptance)

| ID | Decision | Basis |
|---|---|---|
| VPL-49 | Four regression tests encoded relative-speed assumptions that the DDR3 timing invalidates; their stimulus was adjusted, not their checks. The harts execute every instruction from DDR (the VP has no cache) and the FX1 bus serializes all `MEMCTL_DDR` traffic in FIFO order: a fetch costs about 36 ns instead of 19 ns, and behind an ISP ECC write burst (100–200 ns, MAS companion read) the CPU slows down up to 9×. **FreeRTOS CLI** (3 tests, plus the SDK `run_vp.sh`): console replay at the 115200 8N1 character time (`--uart0-rx-byte-us 87`) starting 60 ms after CPU release; at 10 µs per byte the 16-byte RX FIFO overruns while the CLI prints a line with interrupts masked, and at 20 ms the replay started during the 26 ms boot. **ISP abort phases** (`isp_recovery` C/D/E, `isp_mas0710` stream with phase B): ISP DMA bursts of 2 beats (`ISP_ABORT_MAX_BURST_BEATS`) so the CPU reaches the abort point within the frame: 12 µs against 31–37 µs of input read (with 16 beats: 18.5 µs against 16 µs). The burst length changes no data or completion order | User decision 2026-10-15 (adjust the harness/tests); measurements in the acceptance report |
| VPL-50 | **Preinit sequence and CPU release.** The platform issues the programming-guide sequence itself, through a CSR multiplexer in front of `csr_socket`: timing, DFI, INIT waits (MAS tCK values, conservative), topology (2 ranks, 16 rows, 1 024 words, linear), the three commits and `GLOBAL_CTRL.enable`. It waits for mission readiness (scrub included), writes the staged images with `debug_write`, then releases the harts. A release gate between each hart's port adapter and the bus holds bus accesses until then (a BootROM entry would otherwise run during the platform's CSR sequence), and the simulation pauses so that `--timeout-ms`, `--uart0-rx-delay-us` and `--ddr3-fault` times count from the release. The UART host input is held until the release | Plan §5.3 (scrub → preload → CPU; same validation as firmware) |
| VPL-51 | **The platform loads the firmware ELF**, not the hart: BootROM and SRAM segments are written at elaboration, DDR segments are staged for the preinit preload and refused with an explanation in firmware boot. Every hart resets at the ELF entry (explicit `reset_pc`) | Plan §5.2 note (no silent DDR load before step 3) |
| VPL-52 | **DDR_SYSCTRL** (VP SoC glue, `fx1/fx1_ddr_sysctrl.h`): STATUS mirrors the readiness pins and IRQ; PWR_REQ runs the 4-phase P-channel handshake; AUTO_WAKE (reset 1) answers PACTIVE in a low-power state with an ACTIVE request, because firmware executing from DDR cannot issue the wake itself; QOS_LUT/QOS_CFG and RSV_* drive the QoS sideband; RST_CTRL sequences the cold reset and the coordinated core reset (request → reset-safe within 1 ms → pulse), the recovery path that §7.2/§7.3 of the programming guide require from the platform. No fault injection (DEC-04) | DEC-04; HAS §7.1 |
| VPL-53 | **DEC-07 realization.** The FX1 bus removes the base of every terminal target and has no per-target switch. `GenericAxiBridge` adds `FX1_DDR_BASE` back (`address_offset`), so port 0 sees global AXI addresses as DEC-07 requires; host dumps and fault addresses use user offset = global address − `FX1_DDR_BASE` (bootstrap map, or a region 0 based there) | DEC-07 |
| VPL-54 | **Host fault CLI** `--ddr3-fault T_US:KIND[:ARG]`, timed from CPU release: stored-bit flips (`ce`/`ue`/`check` at a global address), read-return faults, write-result faults, the fatal sources, refresh blocking and PHY failure. CPU tests receive the same schedule as compile-time defines | DEC-04 (faults from host/CLI only) |
| VPL-55 | The map keeps `FX1_DDR3_CSR_BASE` and names the bus window `FX1_DDR3_CSR_APERTURE_SIZE`: the IP header already defines `FX1_DDR3_CSR_SIZE` (register space), and both headers meet in firmware. A static assertion keeps them equal | Header coexistence |
| VPL-56 | Host-cost reductions with identical behaviour (same simulated time to the picosecond on the reference workload): the SECDED syndrome from seven precomputed column masks (verified against the previous loop on 5.66 M cases), one prebuilt event-or-list for the data-path waits, reserved capacity in the atom planner. Host time −17 % on `perf_loop` | VPL-48 (measure; improve where free) |
| VPL-57 | **Firmware-init boot images.** `crt0_rom.S` + `fx1_rom.ld`: code, read-only data and the initial `.data` in BootROM, `.data`/`.bss`/stacks in SRAM. A secondary hart leaves its WFI loop only with MSIP pending and the release flag set (SRAM holds no defined value before hart 0 copies `.data`). The BootROM loader (`fw/fx1_soc/boot`) embeds a stripped application ELF, brings the controller up, copies the PT_LOAD segments into DDR and waits for hart 1's acknowledgement before both harts enter the application, so the application's own release IPI cannot be consumed by the loader's | Plan §5.2 steps 1–5 |

Subtask 5 review (`plan/DDR3_VP_SUBTASK5_REVIEW.md`), three findings accepted:

| Finding | Resolution |
|---|---|
| S5-R1 a denied automatic wake was retried at the same instant for ever (recovery required after a failed wake): simulated time stopped, `--timeout-ms` and RST_CTRL could not act | VPL-58: a denied automatic wake is retried only after a back-off in simulated time (1 µs doubling to 1 ms), whatever the cause, persistent or transient; PWR_STATUS[3] reports it; an accepted handshake or any RST_CTRL sequence clears it; firmware requests and resets end the back-off wait at once |
| S5-R2 a CPU reservation survived a cold reset and the startup scrub | VPL-59: the controller reports every change of what DDR addresses hold or designate through `set_storage_listener` (core/cold reset, initialization restart, scrub start, region install); `fx1_soc` drops all DDR reservations of the platform monitor there. CPU test `ddr3_atomic_lifecycle` covers cold reset, core reset, CSR-driven reinitialization and region commit |
| S5-R3 the CPU monitor treated two region aliases of one location as different storage | VPL-60: the platform monitor compares canonical storage names: `Ddr3Subsystem::canonical_ranges()` maps a decoded DDR address to `2^40 + user offset` from the effective map and splits ranges at region boundaries; undecoded and non-DDR addresses keep their bus name. Applies to reservations, SC checks, SC/AMO brackets and the DMA write guards. Tests: monitor unit test (aliases), `fx1_ddr3_subsystem_canonical`, CPU test `ddr3_atomic_alias` (hart store, DMA write, AMO against DMA through aliases) |

Second review (`plan/DDR3_VP_SUBTASK5_REVIEW_R2.md`): S5-R1, S5-R2 and S5-R3 closed, no new finding. Subtask 5 accepted within the reviewed scope (2026-10-15); with it all five subtasks are accepted. The reviewer did not rerun the packaging gate, the mutation suite or sanitizers in that round.
