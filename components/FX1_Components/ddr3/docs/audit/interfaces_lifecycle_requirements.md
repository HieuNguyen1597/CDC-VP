# DDR3 Controller VP — P0 Requirements Audit: Interfaces, CSR Behavior, Lifecycle

Phase: P0 (requirements audit). Status: draft for review. Authority order: HAS v1.0 > MAS v0.1 > RTL.

## 1. Scope and sources read

| Source | Sections | Lines read |
|---|---|---|
| HAS `FTEL_IP_DDR3_CONTROLLER_HAS_v1.0.md` (normative) | §1.5 public interfaces, §1.6 | L690–L750 |
| HAS | §2.8 BE05/BE06 statement (cross-check only) | L875–L882 |
| HAS | §7.1 clock/reset, §7.2 PHY crosswalk, §7.3 profile boundaries | L2860–L3122 |
| HAS | §7.6 top-level interfaces | L3945–L3967 |
| HAS | §8.1–§8.6 CSR, decode, endpoints, mutex/snapshot, IRQ, status | L3968–L5008 |
| HAS | §9.1–§9.5 operation (init, training, refresh, power, status, flow) | L5009–L5651 |
| HAS | Appendix B first review | L5681–L5697 |
| HAS | FE10/FE11/BE15/RS0x one-line cross-checks (grep only) | L1335, L1442, L2077, L2699 |
| MAS `FTEL_IP_DDR3_CONTROLLER_MAS_v0.1.md` (supplementary) | §1–§5 | L769–L1920 (base64 image lines skipped) |
| MAS | §6.1.14 port quiesce/soft reset, §6.1.15 map publication (blocker/op codes only) | L3062–L3222 |
| MAS | §6.2.1 admission terms, §6.2.10 fault table, §6.2.12 adapter faults, §6.2.20 scrub admission (layout/code cross-checks only) | L3295–L3312, L4483–L4517, L4824–L4841, L5250–L5256 |

MAS chapters 7–9 (pins, CSR map, maintenance) are missing. Nothing below is inferred from them. MAS references such as "Chapter 8 lists all of them" are treated as unavailable.

Fixed decisions (taken as given): DEC-04, DEC-05, DEC-06, DEC-07, DEC-08, SPEC-03. Status words with no bit layout read 0 (unavailable).

VP abstraction legend:
- **MODEL**: the VP implements the behavior and checks it in a test.
- **APPROX**: the function is implemented, but at event or transaction granularity instead of cycle-exact timing.
- **ABSTRACTED**: the behavior collapses into the TLM/functional model. The reason is given in the Notes column.
- **OUT**: not implemented. The profile excludes it, or a decision removed it.

Line references: `H:Lnnnn` = HAS line, `M:Lnnnn` = MAS line.

---

## 2. Requirement table

### 2.1 Public interfaces (IF)

| Req ID | Source | Requirement (one testable statement) | VP class | Proposed VP test ID | Notes |
|---|---|---|---|---|---|
| IF-1.5-01 | H:L706–707, H:L4055–4059, H:L4066 | The CSR target accepts 16-bit byte addresses, 32-bit data and 4 byte strobes. It returns OKAY, SLVERR or DECERR, and all mapped words are naturally 4-byte aligned. | MODEL | VP-CSR-01 | TLM target socket with byte-enable pointer. The independent AW/W channels and their acceptance-order pairing are ABSTRACTED: one TLM write carries both. |
| IF-1.5-02 | H:L3976, M:L1052, M:L1103 | The CSR port has one read outstanding and (MAS) one write outstanding. Response payloads stay stable under backpressure. | APPROX | VP-CSR-02 | A blocking `b_transport` serializes per initiator. The model must still let a read complete while a commit write is blocked waiting for its destination ack (two initiators or a non-blocking path). |
| IF-1.5-03 | H:L702–705, H:L3959, H:L3046 | Memory ports: up to 4 AXI4 ports with 32-bit address and 4-bit ID. W_AXI = W_DFI (128 native ratio 2, 64 legacy ratio 1) and strobes = W_AXI/8. | MODEL | VP-CFG-01 | Only admission/backpressure is in this audit's scope. |
| IF-1.5-04 | H:L709, H:L2933 | The reset/quiesce bundle has core and CSR reset_req/reset_safe (1 bit each), plus a 4-bit port_quiesce_req, port_quiesce_ack and port_soft_reset. | MODEL | VP-LIFE-01 | sc_signal levels. DEC-04 does not name a driver for these: GAP-LC-15. |
| IF-1.5-05 | H:L710, M:L903, M:L3190–3192, DEC-07 | The bootstrap map (4 ports × 4 regions × 32-bit base/size) is the active decode image from cold reset until the first successful FE_REGION_COMMIT. The lowest matching region wins, and SIZE=0 disables an entry. | MODEL | VP-CFG-10 | Readback of FE_REGION_ACTIVE under DEC-07: GAP-CSR-14. |
| IF-1.5-06 | H:L715, H:L3039, H:L5327 | Training load: three classes (read-level, gate, write-level), each with one strobe and a 32-bit payload. One transaction is outstanding, and the payload stays stable until the PHY ack. | ABSTRACTED (functional provider call `load(rank,epoch,tag,class,payload)` → ack/timeout/stale/fail) | VP-BOOT-05 | Pin strobes are not modeled. The provider can inject success, fail, timeout or stale results. |
| IF-1.5-07 | H:L716, H:L3040, H:L5328 | Observations (8-bit read-level, 8-bit write-level, 3×2-bit mode) are tied to the sole outstanding rank/epoch/tag. Mode fields never gate readiness. | APPROX | VP-BOOT-06 | Stored opaque. |
| IF-1.5-08 | H:L717, H:L3964, M:L1064 | Status outputs are 1-bit levels: mission_ready, global_idle, fe_drained, and an active-high level IRQ. | MODEL | VP-LIFE-02, VP-IRQ-01 | DEC-04: DDR_SYSCTRL mirrors these together with init_complete. |
| IF-1.5-09 | H:L723–724 | QoS programming sideband per port: we, index[3:0], class[1:0], cfg write/value, configured out. Reserved-rate sideband: we, port[1:0], rate[19:0], depth[19:0]. Both are in the core domain. | MODEL | VP-CFG-20 | DEC-04: DDR_SYSCTRL drives both. Contradicts MAS "no sideband" (C-14). |
| IF-1.5-10 | H:L725, H:L3027 (RS06) | PHY reset_n is active-low and PHY clock_disable is active-high. The clock stays enabled through init, traffic and mandatory exit waits, and is disabled only after a completed low-power transition permits gating. | APPROX | VP-POWER-10 | Levels go to the provider. Gating has no timing effect in the VP. MAS L1147: DRAM clock stops only in self-refresh. |
| IF-1.5-11 | H:L726, H:L3038 (BE07), H:L5226 | PHY reset_done, ready, failure and timing_wait_ok are separate active-high levels. Init advances only when reset_done and ready are both 1. failure takes priority and blocks admission (sticky). timing_wait_ok gates the corresponding timed transition. | MODEL | VP-BOOT-02, VP-BOOT-03 | No synchronizer latency (CDC not modeled). Which transition timing_wait_ok gates is unstated: GAP-LC-05. |
| IF-1.5-12 | H:L727, H:L3018, H:L3041 (BE10) | Update observation inputs (ack, req, type[1:0]) are captured only. No live-update transaction is accepted, and CAP_FEATURE[9]=0. | MODEL (tie-off) | VP-CFG-02 | MAS live phyupd/ctrlupd support is OUT (C-10). |
| IF-1.5-13 | H:L728–730, H:L3042 (BE08), H:L5230 | init_complete is a controller output. It goes high after successful DRAM init plus training, and clears on reset, reinit or failure. It does not depend on scrub, and no PHY-generated init_complete input exists. | MODEL | VP-BOOT-01 | MAS dfi_init_complete/dfi_init_start handshake is OUT (C-12). |
| IF-7.2-01 | H:L3046, H:L3056, H:L3953 | W_DFI = TOTAL_DQ_BITS×2×FREQUENCY_RATIO. PHY clock = controller clock × ratio. Rank count does not multiply width. | MODEL (parameters) | VP-CFG-03 | Elaboration check. |
| IF-7.2-02 | H:L3056 (BE13), M:L1737 | Every delay lives in a named clock domain. A minimum of n PHY/tCK cycles converts to ceil(n/R) controller cycles. A phase-exact event that cannot be represented rejects the profile. | APPROX | VP-CFG-04 | VP converts with sc_time = n·tCK and applies ceil to the core period. |
| IF-7.2-03 | H:L2960, H:L2990–2994, H:L3062 | A malformed, partial, extra, late or tokenless read return triggers global read containment. Config requires read_timeout > PHY_max_return + assembly_margin. Recovery needs a coordinated controller+PHY reset. | APPROX | VP-LIFE-30 | Provider "timeout/stale" injection → quarantine. Layout: GAP-CSR-12. |
| IF-7.2-04 | H:L3012–3016, H:L3028–3037 (BE12/BE14/RS07) | DFI phase, rank, CS/CKE/ODT, mask-inversion and chronological slice mapping. | ABSTRACTED (transaction-level PHY provider, no pin-level DFI) | — | Covered by the data-path audit, not this one. |
| IF-7.3-01 | H:L3090, H:L3962, H:L3966, H:L5139 (RD-PWR-01) | P-channel: PREQ and PSTATE[1:0] in; PACCEPT, PDENY and PACTIVE out. PACCEPT/PDENY are held until PREQ is low, and a new request starts only after both responses are low. PSTATE 00 = active/wake, 01 = active power-down, 10 = precharge power-down, 11 = all-active-rank self-refresh. | MODEL (4-phase on sc_signal; pclk_ao mailbox ABSTRACTED) | VP-POWER-01 | DEC-04: DDR_SYSCTRL drives PREQ/PSTATE. |
| IF-7.3-02 | H:L3101, H:L3109, H:L4835 | The lifecycle snapshot bank is 30×32-bit words + valid + 16-bit sequence. | APPROX | VP-CSR-30 | Word composition is undefined: GAP-CSR-20. |
| IF-7.3-03 | H:L3091–3092, H:L3119 (BE17), H:L5135 (BE06) | Excluded: live DFI update, retraining without quiesce/reinit, autonomous eye search/sweep, fine-granularity and temperature-compensated refresh, partial-rank SR (RD-PWR-05). | OUT | VP-CFG-02 (negative) | |
| IF-7.6-01 | H:L3964 | The CSR contract requires no extra public status pins. Readiness, idle and lifecycle status are available through the CSR. | MODEL | VP-LIFE-02 | |

### 2.2 Clock, reset and domains (LC-7.1)

| Req ID | Source | Requirement | VP class | Proposed VP test ID | Notes |
|---|---|---|---|---|---|
| LC-7.1-01 | H:L2869, H:L2917–2923, M:L1274–1280 | Domains: core (clk/rst_n), CSR (csr_aclk/csr_aresetn), 4 AXI ports, always-on mailbox (cdc_por_n), and P-channel (pclk_ao/preset_n). | ABSTRACTED (one SystemC kernel; domain clocks are only periods for timing conversion; no CDC/metastability) | VP-CFG-05 | Fixed VP abstraction. |
| LC-7.1-02 | H:L2873, H:L2881–2885 | reset_safe = reset_request ∧ reset_released ∧ local_idle ∧ endpoint_aligned ∧ online. A reset request first stops admission and waits for owned work and response state to drain. | APPROX | VP-LIFE-03 | endpoint_aligned and online are always true in the VP. |
| LC-7.1-03 | M:L1063, M:L1288, M:L1127 | o_core_reset_safe also waits until every enabled port acknowledges quiesce (MAS; HAS silent → adopt). | APPROX | VP-LIFE-03 | Marked MAS-adopted. |
| LC-7.1-04 | H:L2875, M:L3121 | A per-port warm/soft reset never deletes backend-owned work. It starts only after that port's quiesce ack and holds for 4×SYNC_STAGES = 8 clk cycles (MAS). | MODEL | VP-LIFE-10 | Hold is modeled as 8 core periods. |
| LC-7.1-05 | H:L2877, H:L1442 (FE10) | Emergency/global recovery clears lifecycle state. Init, training and scrub must all rerun before admission. No live backend-only reset that deletes accepted commands is allowed. | MODEL | VP-LIFE-04 | |
| LC-7.1-06 | H:L2946 (RD-RST-01) | The reset-request acknowledgement timeout and recovery for external reset sources are "proposed" without values. | — | — | GAP-LC-14. |
| LC-7.1-07 | H:L2948 | The external reset controller owns assertion policy, and retention across resets is not promised. | MODEL | VP-LIFE-04 | |
| LC-7.1-08 | M:L1264, M:L1858 | Integration rule: the aclk_s[p] period must not exceed 2× the clk period (not checked at elaboration). | APPROX | VP-CFG-05 | The VP warns at elaboration. |

### 2.3 CSR decode, access classes, strobes (CSR-8.1 / CSR-8.2)

| Req ID | Source | Requirement | VP class | Proposed VP test ID | Notes |
|---|---|---|---|---|---|
| CSR-8.2-01 | H:L4569, H:L4583–4587, H:L4598 | Decode: aligned = (addr mod 4 = 0), region = floor(addr/4096), word = floor(addr/4) mod 1024. A misaligned access (for example 0x0102) returns SLVERR. | MODEL | VP-CSR-03 | Misaligned access to an unmapped word: GAP-CSR-01. |
| CSR-8.2-02 | H:L4028, H:L4088, H:L4627 | An unlisted address or reserved word returns DECERR with zero read data and no side effect. Exactly 210 words in 7 regions (0x0–0x6) are decoded. | MODEL | VP-CSR-04 | Sweep all 16K word addresses against Table 8-5. |
| CSR-8.2-03 | H:L4092, H:L4626 | A write to an RO register returns SLVERR and leaves the register unchanged. | MODEL | VP-CSR-05 | |
| CSR-8.2-04 | H:L4092 | RW, SHADOW and INIT_RW writes merge only the selected byte lanes. WSTRB=0 is a legal no-op (OKAY). | MODEL | VP-CSR-06 | |
| CSR-8.2-05 | H:L4092, H:L4573, H:L4619 | W1C and WO-CMD writes need WSTRB=0xF. A partial write returns SLVERR with no side effect (for example, W1C with WSTRB=0001 clears nothing). | MODEL | VP-CSR-07 | |
| CSR-8.2-06 | H:L4092 | A read of a WO-CMD register returns 0 with OKAY. | MODEL | VP-CSR-08 | IRQ_CLEAR is W1C with "no storage", so its read value is undefined: GAP-CSR-02. |
| CSR-8.2-07 | H:L4092, H:L4571, H:L4626 | Reserved bits marked "write zero" must be 0, or the write returns SLVERR with no effect. Bits marked "writes are ignored" or "writes have no effect" are silently dropped. | MODEL | VP-CSR-09 | Write-zero fields: GLOBAL_CTRL[31:2], QUIESCE_CTRL[31:2], SNAPSHOT_CMD[31:1], FE_PORT_CMD[31:10,7:3], PHY_RDLVL/GATE[31:24], PHY_WRLVL[31:3], TRAIN_CMD[31:28]. Ignored fields: RESET_CAUSE[31:8], COMMAND_ERROR[31:3], INIT_ERROR[31:16], POWER_ERROR[31:24], PHY_BRIDGE_ERROR[31:8]. The split is an interpretation: GAP-CSR-03. |
| CSR-8.2-08 | H:L4092, H:L4110 | A failed validation or interlock returns SLVERR and leaves the previous value unchanged. | MODEL | VP-CSR-10 | |
| CSR-8.2-09 | H:L4575, H:L4651 | Error classification has no side effects, including command collisions and access to disabled optional features. Destination busy and lifecycle errors are separate SLVERR conditions. | MODEL | VP-CSR-11 | Tension with sticky COMMAND_ERROR: GAP-CSR-04. |
| CSR-8.2-10 | H:L4577 | When a read and a write hit the same register in one cycle, the read sees the pre-write value. | APPROX | VP-CSR-12 | TLM order of arrival decides. |
| CSR-8.2-11 | H:L4628, H:L4559, M:L1175 | When an optional feature is absent, reads return zero/OKAY and writes return SLVERR as each register's contract says. IRQ_TEST_SET returns SLVERR when test injection is disabled. | MODEL | VP-CSR-13 | |
| CSR-8.2-12 | H:L4094, H:L4276, H:L4870 | For every W1C sticky register, a hardware set in the same cycle beats a software clear. | APPROX | VP-IRQ-03 | The VP applies the HW set after the W1C inside the same delta/timestamp. |
| CSR-8.1-01 | H:L4106–4110, H:L4229–4249 | IP_ID = 0x44445233, ABI_VERSION = 0x00010000, CAP_DEPTH0 = 0x04101008, CAP_WIDTH0 = 0x00208410 (native) or 0x00204410 (legacy). CAP_FEATURE[31:12] = 0 and CAP_FEATURE[9] = 0. | MODEL | VP-CSR-14 | Field arithmetic checked: CAP_DEPTH0 → 8/16/16/4; CAP_WIDTH0 → 16/32/16(8)/4. CAP_FEATURE value: GAP-CSR-15. |
| CSR-8.1-02 | H:L4119, H:L4276 | SCRATCH is RW, byte-mergeable, resets to 0, and has no side effect. | MODEL | VP-CSR-15 | |
| CSR-8.1-03 | H:L4257–4259, M:L3304 | GLOBAL_CTRL[0] enable (reset 0) is a mission-admission term (MAS-adopted). GLOBAL_CTRL[1] is the software admission stop, the "software_stop" term of admission_block. | MODEL | VP-LIFE-05 | Software must set enable before any traffic is admitted. |
| CSR-8.1-04 | H:L4112, H:L4563 | GLOBAL_STATUS reads 0x00000000 in this profile. | MODEL | VP-CSR-16 | |
| CSR-8.1-05 | H:L4310–4311, H:L4317 | RESET_CAUSE reads zero in this profile. RD-RST-02 codes (0 none, 1 cold/POR, 2 software, 3 watchdog, 4 PHY, 5 fatal recovery) are only a proposal. | MODEL (reads 0) | VP-LIFE-06 | GAP-LC-13. |
| CSR-8.1-06 | H:L4312–4317 | COMMAND_ERROR is sticky W1C: [0] busy/mutex collision, [1] blocked mailbox/interlock result, [2] commit validation/interlock failure. Each bit has a distinct HW set path. | MODEL | VP-CSR-17 | Set rules: GAP-CSR-04. |
| CSR-8.1-07 | H:L4410, M:L1878 | WRITE_DRAIN_CFG, STARVE_LIMIT and READ_TIMEOUT form one validated policy image. An invalid value returns SLVERR and keeps the previous image. | MODEL | VP-CSR-18 | MAS ranges adopted: enter 1..REQ_DEPTH, exit 0..enter−1, max ≥1, STARVE_LIMIT 1..2^31−1. |
| CSR-8.1-08 | H:L4047, H:L4418, H:L4463, H:L4483, H:L4522 | Live policy registers (REFRESH_CFG, POWER_TIMEOUT, CE_THRESHOLD, TRAIN_TIMEOUT, READ_TIMEOUT) publish the validated value before OKAY. Nonzero rule: POWER_TIMEOUT, CE_THRESHOLD, TRAIN_TIMEOUT, READ_TIMEOUT, STARVE_LIMIT ≠ 0. REFRESH_CFG[3:0] ∈ 1..8. | MODEL | VP-CSR-19 | A partial-strobe write merges first and validates the merged word (proposal; HAS silent). |

### 2.4 Commit protocol (CSR-8.1 / CSR-8.3)

| Req ID | Source | Requirement | VP class | Proposed VP test ID | Notes |
|---|---|---|---|---|---|
| CSR-8.1-10 | H:L3990–4002 | commit_accept = command_valid ∧ image_legal ∧ quiesce_ok ∧ idle_ok ∧ source_ready. On accept, the control image changes first. The destination image changes only on a matching install acknowledgement (destination_accept ∧ payload_valid ∧ identity_match). | APPROX | VP-CFG-30 | The destination ack is a scheduled event, so it can be delayed and stale-injected. |
| CSR-8.1-11 | H:L4094, H:L4381 | A commit write (TOPO_COMMIT, TIMING_COMMIT, DFI_TIMING_COMMIT, FE_REGION_COMMIT) validates and freezes the full image. Its B response returns only after the destination acknowledgement: OKAY on success. | MODEL | VP-CFG-31 | The B response can block for a long time (FE_REGION waits for port drain). |
| CSR-8.1-12 | H:L4354, H:L4362, H:L4381, H:L4385–4388 | Immediate rejection returns SLVERR, publishes no candidate, keeps the active image, sets STATUS.error=1, and writes the blocker to [23:16]. | MODEL | VP-CFG-32 | |
| CSR-8.1-13 | H:L4353, H:L4361, H:L4507, H:L4385–4390 | A failure on the acknowledged-completion path writes the blocker to [15:8]. After any SLVERR commit, software checks both [15:8] and [23:16]. | MODEL | VP-CFG-33 | Whether this path returns SLVERR is implied, not stated: GAP-CSR-05. |
| CSR-8.1-14 | H:L4529 | DFI_TIMING_STATUS uses the same blocker locations, [15:8] and [23:16]. | MODEL | VP-CFG-34 | |
| CSR-8.1-15 | M:L1869, M:L1881, M:L3202 | Known blocker codes (MAS; HAS has none): 0x11 = TOPO_COMMIT failure (active geometry kept). 0x04 = TIMING_COMMIT failure (meaning not given). 0x0B = region crossing not ready. 0x0C = region alignment / BASE+SIZE > 2^32 / same-port overlap. 0x0D = region end beyond USABLE_GRANULES. | MODEL (adopt MAS codes) | VP-CFG-35 | The full code table is in MAS Ch.8, which is missing: GAP-CSR-06. |
| CSR-8.1-16 | H:L4352, H:L4360 | TOPO_COMMIT_STATUS[2] and TIMING_COMMIT_STATUS[2] report "reinit required". | APPROX | VP-CFG-36 | Set/clear rule undefined: GAP-CSR-07. |
| CSR-8.1-17 | H:L4364, SPEC-03 | TOPO_SHADOW, the 19 timing shadow words, TOPO_ACTIVE and the A_* mirrors reset to 0x00000000. The mirrors are RO and show the committed image after a successful install. | MODEL | VP-CFG-37 | So TIMING_COMMIT straight after reset fails validation (C-05). |
| CSR-8.1-18 | M:L1868 | TOPO_COMMIT validation (MAS-adopted): quiesced; active ranks ≤ build max; row bits 12..ROW_BITS_MAX; legal page code; map profile in the build mask; physical capacity ≤ 2^27 granules; with ECC, the build has ECC and ≥1 usable granule. | MODEL | VP-CFG-38 | Page code 0..3 = 256/512/1024/2048 words (H:L4344, consistent with MAS images 0x501/0x6F1). |
| CSR-8.1-19 | M:L1870–1872 | TIMING_COMMIT validation (MAS-adopted, limited to the 19 HAS words): non-zero where JEDEC has a minimum; < 2^(BE_TIME_W−1); CL 5..11; CWL 5..8; T_WR 5..16; AL ∈ {0, CL−1, CL−2}; T_RTW ≥ CL+6−CWL; T_CCD, T_RRD, T_RTP, T_WTR ≥ 4. | MODEL | VP-CFG-39 | Floors for the DEC-08 words (T_CKE..T_ZQCS) do not apply. |
| CSR-8.1-20 | H:L4529, M:L1874–1876, H:L2994 | DFI_TIMING_COMMIT checks validation plus an idle interlock: no read token pending, adapter idle, DFI_ASSEMBLY_MARGIN ≥ 1, READ_TIMEOUT > PHY_MAX_READ_RETURN + DFI_ASSEMBLY_MARGIN. | MODEL | VP-CFG-40 | MAS checks on the DEC-08 DFI words do not apply. |
| CSR-8.1-21 | H:L4325–4334, M:L3202–3206, M:L3214 | FE_REGION_COMMIT validates the enabled entries (alignment, same-port overlap, overflow, and capacity per MAS), freezes all 32 words, raises region quiesce on every port, waits for drain and endpoint ack, then installs. It keeps the previous image on failure. Different ports may overlap. FE_REGION_STATUS fields: busy[0], done[1], error[2], blocker[15:8], active valid[16]. | MODEL | VP-CFG-11 | Index i = 2*(4p+r)+k. Backend admission stays open during region quiesce (MAS). |
| CSR-8.1-22 | H:L4334 | Core offline clears the FE_REGION_ACTIVE mirror and requires a fresh commit. | MODEL | VP-CFG-12 | Whether decode falls back to the bootstrap map: GAP-CSR-14. |
| CSR-8.1-23 | H:L4390 (RS08) | USABLE/PHYSICAL_GRANULES and ECC_BASE are zero-extended 64-bit values. _HI holds bits [63:32] of the same snapshot, which are 0 for the 32-bit profiles. ECC_BASE = 0 without ECC. | MODEL | VP-CFG-41 | Snapshot-backed. |
| CSR-8.1-24 | DEC-05, H:L5145, H:L5648, M:L1040 | Each successful TOPO, TIMING or DFI commit restarts full initialization. It clears mission_ready and init_complete, and restarts scrub from granule 0. | MODEL | VP-BOOT-10 | BE05's MR/PHY-change triggers: GAP-LC-03. |
| CSR-8.1-25 | H:L4663, H:L4760 | A second source update waits until the one-entry source capacity is released. A reset during a pending transfer discards or replays the whole transaction and never publishes a partial image. | APPROX | VP-CFG-42 | With serialized commands only one commit can be pending. |
| CSR-8.1-26 | H:L4665, H:L4705 | A stale result (epoch/tag mismatch) is rejected or consumed without changing lifecycle state. | MODEL | VP-CFG-43 | Injected through the delayed destination ack. |

### 2.5 Mutex, commands, snapshots (CSR-8.4 / CSR-8.6)

| Req ID | Source | Requirement | VP class | Proposed VP test ID | Notes |
|---|---|---|---|---|---|
| CSR-8.4-01 | H:L3980, H:L4766–4768 | The command mutex covers quiesce, topology, timing and DFI commits, init, training, power, and frontend lifecycle (FE_PORT_CMD). A command that arrives while the mutex is busy returns SLVERR and is not queued. | MODEL | VP-CSR-20 | FE_REGION_COMMIT membership: GAP-CSR-08. |
| CSR-8.4-02 | H:L4780–4784 | command_accept = request ∧ ¬mutex_busy ∧ interlocks_ok. The owner keeps the mutex until its matching result is consumed. | MODEL | VP-CSR-21 | |
| CSR-8.4-03 | H:L4423–4424, H:L4468, H:L5155, DEC-05 | INIT_CMD[0] start and INIT_CMD[1] retry writes return SLVERR and launch nothing. | MODEL | VP-BOOT-11 | |
| CSR-8.4-04 | H:L4457, H:L4468, M:L1163 | A POWER_CMD[1:0] write returns SLVERR and launches nothing. Power is requested only through the P-channel. | MODEL | VP-POWER-02 | |
| CSR-8.4-05 | H:L4270–4274, H:L4766, H:L4772 | SNAPSHOT_CMD[0] requests a coherent capture through a separate busy/ack path (not the mutex). A capture request while SNAPSHOT_STATUS.busy=1 returns SLVERR. | MODEL | VP-CSR-31 | B-response timing: GAP-CSR-09. |
| CSR-8.4-06 | H:L4096, H:L4803, H:L4816 | SNAPSHOT_STATUS.valid=0 until the first acknowledged capture. Before that, every snapshot-backed word reads 0. | MODEL | VP-CSR-32 | INIT_STATUS, POWER_STATUS and others read 0 until software takes a snapshot (§3). |
| CSR-8.4-07 | H:L4774, H:L4803–4805 | A captured bank stays stable until the next acknowledged capture. valid requires capture_ack ∧ sequence match. Reset/recovery invalidates a pending capture, and a partial capture is never valid. | MODEL | VP-CSR-33 | |
| CSR-8.4-08 | H:L4274, H:L4843 | SNAPSHOT_STATUS[31:16] is a monotonic sequence. Words wider than 32 bits use LO/HI halves captured under the same sequence. | MODEL | VP-CSR-34 | First value after reset: GAP-CSR-09. |
| CSR-8.6-01 | H:L4936, H:L5003 | A snapshot is an observation boundary. It does not pause traffic, maintenance, counters or IRQ. | MODEL | VP-CSR-35 | |
| CSR-8.6-02 | H:L4932, H:L5007, H:L5561 | A producer missing from the profile reads its documented zero, and software must not read that as a measured value. | MODEL | VP-CSR-16 | Applied to the layout-less words (decision). |

### 2.6 Interrupts (CSR-8.5)

| Req ID | Source | Requirement | VP class | Proposed VP test ID | Notes |
|---|---|---|---|---|---|
| CSR-8.5-01 | H:L4541–4552 | IRQ_RAW sources: [0] CE threshold, [1] ECC UE, [2] physical read timeout, [3] read-return quarantine, [4] mapping/config, [5] refresh emergency, [6] init failure, [7] training failure, [8] power transition failure, [9] PHY alert/failure, [10] CSR command/interlock, [11] fatal internal. | MODEL | VP-IRQ-02 | The HW trigger for [4], [5], [9] and [10] is not defined: GAP-CSR-10. |
| CSR-8.5-02 | H:L4871–4875, H:L4559 | raw_next = (raw ∧ ¬w1c) ∨ hw_set ∨ test_set; IRQ_STATUS = IRQ_RAW & IRQ_MASK; irq = OR(IRQ_STATUS). Masking never clears raw state. All IRQ registers reset to 0. | MODEL | VP-IRQ-01 | |
| CSR-8.5-03 | H:L4555, H:L4914 | IRQ_CLEAR is a W1C clear request that needs WSTRB=0xF. Only bits [11:0] have an effect. | MODEL | VP-IRQ-04 | |
| CSR-8.5-04 | H:L4556, H:L4920, M:L1586 | IRQ_TEST_SET (full word) sets raw bits only when test injection is enabled (default build enabled; CAP_FEATURE[11]). Otherwise it returns SLVERR. It never creates ECC or PHY metadata. | MODEL | VP-IRQ-05 | |
| CSR-8.5-05 | H:L4865 | Clearing an IRQ does not acknowledge or discard the underlying lifecycle result or sticky error. | MODEL | VP-IRQ-06 | |
| CSR-8.5-06 | M:L1919, H:L4483 | IRQ_RAW[0] is set once each time the 32-bit saturating CE_COUNT reaches the (nonzero) CE_THRESHOLD. | MODEL (MAS-adopted) | VP-IRQ-07 | HAS forbids 0, while MAS says 0 = off (C-09). |
| CSR-8.5-07 | M:L4485, M:L4491–4496 | Each fatal event sets FATAL_STATUS and IRQ_RAW[11]. Read-quarantine reason 0 (timeout) sets IRQ_RAW[2], and reasons 1–4 set IRQ_RAW[3]. | MODEL (MAS-adopted) | VP-IRQ-08 | |

### 2.7 Global quiesce, frontend port commands, region map (CSR-8.1)

| Req ID | Source | Requirement | VP class | Proposed VP test ID | Notes |
|---|---|---|---|---|---|
| CSR-8.1-30 | H:L4260–4262, H:L4276 | QUIESCE_CTRL[0] requests global quiesce and [1] releases it. It is a full-word WO-CMD, serialized through the mutex. | MODEL | VP-LIFE-20 | Exactly-one-bit rule and B timing: GAP-LC-08. |
| CSR-8.1-31 | H:L4263–4269 | QUIESCE_STATUS fields: [0] request outstanding, [1] admission stopped, [2] global idle, [3] mission idle, [4] DFI tokens empty, [5] complete (request ∧ available idle terms), [6] timeout. | MODEL | VP-LIFE-21 | |
| CSR-8.1-32 | H:L4269 (RD-CSR-02), H:L2077 (FE10) | Quiesce waits indefinitely. QUIESCE_STATUS[6] and FE_PORT_STATE[19:16] stay 0 because no finite timeout is exposed. | MODEL | VP-LIFE-22 | MAS FE_QUIESCE_TIMEOUT is not exposed (C-22). |
| CSR-8.1-33 | H:L4284–4286, H:L4302 | FE_PORT_CMD has op[2:0] and port[9:8]; reserved bits must be 0. The operation/port pair must be valid. The command is serialized, and the B response waits for the matching endpoint result. | MODEL | VP-LIFE-11 | |
| CSR-8.1-34 | M:L3138–3139 | FE_PORT_CMD op 1 = port quiesce and op 3 = port soft reset (MAS-adopted). | MODEL | VP-LIFE-12 | Ops 0, 2 and 4–7 and the blocker codes are undefined: GAP-CSR-11. |
| CSR-8.1-35 | H:L4287–4292, H:L4302 | FE_PORT_CMD_STATUS fields: busy[0], done[1], error[2], completed op[5:3], completed port[9:8], blocker[23:16]. A new legal command clears the previous done/error. | MODEL | VP-LIFE-13 | |
| CSR-8.1-36 | H:L4293–4302 | FE_PORT_STATE ([3:0] CSR quiesce, [7:4] effective quiesce, [11:8] ack, [15:12] reset active, [19:16] timeout, [23:20] reset pending, [27:24] backend drained, [31] valid) is published only on a successful endpoint result. | MODEL | VP-LIFE-14 | Between commands it is not live. |
| CSR-8.1-37 | M:L3068–3092, M:L3115 | Port quiesce: AW/AR ready drop; W owed to accepted AWs is still taken; ack = request ∧ stop_held ∧ residency_empty ∧ backend_drained. Backend admission stays open. | APPROX | VP-LIFE-15 | The 14 residency terms collapse to an "outstanding transactions = 0" check. |
| CSR-8.1-38 | M:L3121, M:L3132, M:L3165 | A soft-reset request without a quiesce ack does not start (MAS: it waits). Soft reset clears the port's QoS table and configured flag. | MODEL | VP-LIFE-16 | With the DEC-04 sideband, DDR_SYSCTRL must reprogram QoS. A command that waits could hang the B response: GAP-CSR-11. |

### 2.8 Initialization (LC-9.1.1)

| Req ID | Source | Requirement | VP class | Proposed VP test ID | Notes |
|---|---|---|---|---|---|
| LC-9.1.1-01 | H:L5147, DEC-05 | Release of cold reset starts init in an admission-blocked hold. AXI4-Lite stays available, and memory requests are backpressured. | MODEL | VP-BOOT-01 | |
| LC-9.1.1-02 | DEC-05, H:L5242 (RS11) | Init leaves hold only after the TOPO, TIMING and DFI active images are all valid. Reset leaves timing_valid low, and no unchecked bootstrap timing tuple exists. | MODEL | VP-BOOT-12 | |
| LC-9.1.1-03 | H:L5149–5151, H:L5208–5217, H:L4470 | Stage order: hold → PHY reset (fixed 4 core cycles) → wait PHY reset_done ∧ ready → DRAM reset-release wait → CKE-low wait → CKE-high wait → MR2 → MR3 → MR1 → MR0 (DLL-reset intent) → DLL wait → ZQCL → ZQ wait → training handoff → ready/failure. | MODEL | VP-BOOT-13 | The provider logs the command order. Exact stage codes: GAP-LC-01. |
| LC-9.1.1-04 | H:L5159–5163 | stage_done = (time ≥ stage_start + configured_wait) ∧ required_handshake. MRS and ZQCL advance only after downstream acceptance plus timing legality. | APPROX | VP-BOOT-14 | Units: GAP-LC-04. |
| LC-9.1.1-05 | H:L4470 | The PHY-reset wait is fixed at 4 core cycles. INIT_RESET_ASSERT_WAIT is kept for readback only and does not control it. | MODEL | VP-BOOT-15 | Whether the DRAM RESET# assert duration is programmable: GAP-LC-04. |
| LC-9.1.1-06 | H:L4455, H:L5153, M:L1161 | INIT_CMD_TIMEOUT is stored and exported but does not act as a watchdog. A permanently rejected MRS/ZQCL stalls init without a timeout. | MODEL | VP-BOOT-16 | MAS "watchdog" wording overridden (C-07). |
| LC-9.1.1-07 | H:L5153, H:L5226 | A PHY failure, an out-of-range active-rank count at start, or a failed boot-training result marks init failed. It records the failing stage, keeps admission blocked, keeps init_complete low, and sets IRQ_RAW[6]. | MODEL | VP-BOOT-17 | Provider fail/timeout injection. |
| LC-9.1.1-08 | H:L4425–4444 | INIT_STATUS fields: [1] dram initialized, [2] failed, [10:7] stage, [15] admission block. All other bits read 0. INIT_STATUS is snapshot-backed. | MODEL | VP-BOOT-18 | Stage encoding: GAP-LC-01. |
| LC-9.1.1-09 | H:L4446, M:L1177 | INIT_ERROR[15:0] holds a sticky one-hot code of the failing stage (W1C; HW set wins). An event reaches it only when exactly one bit is set. | MODEL | VP-BOOT-19 | Bit map: GAP-LC-02. |
| LC-9.1.1-10 | H:L5225, H:L5236, M:L1873 | The MR0–MR3 image stays stable for the whole init attempt. Only representable DDR3 address bits are used. MAS field checks (Rtt_Nom, drive, Rtt_WR≠11, not both SRT and ASR) are adopted as the INIT_RW write validation. | APPROX | VP-BOOT-20 | MAS "fields built from timing words" is ABSTRACTED: the provider sees the MR value as written. |
| LC-9.1.1-11 | H:L4094, H:L4531 | INIT_RW words (MR0–3, INIT_*, TRAIN_TRAS_WAIT, T_CKSRE/X, PHY_* delay/skew) accept byte-merged writes "until the initialization lock". | MODEL | VP-BOOT-21 | Lock point: GAP-LC-06. |
| LC-9.1.1-12 | H:L4165–4172, H:L4468, SPEC-03 | Reset values: INIT_* waits = 1, INIT_CMD_TIMEOUT = 0x400, TRAIN_TRAS_WAIT = 0x14, MR0–3 = 0, T_CKSRE/T_CKSRX = 1. | MODEL | VP-CSR-40 | MAS speed-bin images overridden (C-01..C-04). |
| LC-9.1.1-13 | H:L4456 | TRAIN_TRAS_WAIT must be nonzero (minimum tRAS wait). | MODEL | VP-BOOT-22 | Writing 0 returns SLVERR (proposal). |
| LC-9.1.1-14 | H:L879, H:L5246, H:L5648 | A restart reruns the whole sequence and never keeps partial DDR state. An init failure after publication keeps admission closed. Software then fixes the config and commits again, or does a coordinated reset. | MODEL | VP-BOOT-23 | |
| LC-9.1.1-15 | M:L853, M:L1501 | An inactive rank is skipped by init, training, refresh, ZQ and power, and its CKE/ODT are held low (MAS-adopted). | MODEL | VP-BOOT-24 | |

### 2.9 Training (LC-9.1.2, LC-7.2)

| Req ID | Source | Requirement | VP class | Proposed VP test ID | Notes |
|---|---|---|---|---|---|
| LC-9.1.2-01 | DEC-06, H:L5153, H:L5325 | Boot training runs automatically in the training-handoff stage. For each active rank it loads PHY_RDLVL_DELAY, PHY_GATE_DELAY and PHY_WRLVL_DELAY (plus SKEW, opaque) through the provider with tag 0. | MODEL | VP-BOOT-05 | Transport of SKEW_CTRL0/1: GAP-LC-09. |
| LC-9.1.2-02 | H:L5256, H:L5264, H:L3039 | Only one acknowledged PHY operation is outstanding at a time, and the load payload stays stable until the ack. An episode is one command, one transaction, one observation, one terminal result. | MODEL | VP-BOOT-06 | |
| LC-9.1.2-03 | H:L5266–5272 | episode_done = result_valid ∧ result_epoch = command_epoch ∧ result_tag = command_tag. timed_out = (time − start ≥ timeout) ∧ ¬episode_done. | MODEL | VP-BOOT-07 | Which timeout applies to boot training: GAP-LC-10. |
| LC-9.1.2-04 | H:L5258, H:L3050, H:L716 | A timeout, stale epoch/tag, rejected operation or PHY failure ends the episode with a blocker. Admission stays closed, the previous rank context is kept, and IRQ_RAW[7] is set. | MODEL | VP-BOOT-08 | Injection: timeout, stale, fail. |
| LC-9.1.2-05 | H:L716, H:L3050 | A successful matching observation updates only the selected rank's saved context. | MODEL | VP-BOOT-09 | |
| LC-9.1.2-06 | H:L5293–5297 | training_complete = required_training_done ∧ ¬training_error ∧ epoch_current. Reinit, a profile change or fatal recovery clears it. | MODEL | VP-BOOT-25 | |
| LC-9.1.2-07 | H:L4516–4519, H:L4531, H:L5337 | TRAIN_CMD is a full-word write: [0] must be 1, [3:1] primitive, [11:4] rank/lane, [27:12] options, [31:28] reserved. A zero TRAIN_TIMEOUT invalidates any accepted command. | MODEL | VP-BOOT-30 | Primitive codes and the rank/lane layout are undefined: GAP-LC-11. |
| LC-9.1.2-08 | DEC-06, H:L3058, H:L5254, H:L5326 | Runtime TRAIN_CMD goes through admission stop, global drain, active power and reinit ownership. It trains one rank, saves its delay context, issues one bounded primitive, and reports rank + epoch/tag. training_busy holds mission_ready low. | MODEL | VP-BOOT-31 | Whether the controller performs or only checks these steps: GAP-LC-12. |
| LC-9.1.2-09 | H:L5325 | Tag 0 is reserved for boot training. | MODEL | VP-BOOT-32 | Runtime tags 1..15 (proposal). |
| LC-9.1.2-10 | H:L4520–4523 | TRAIN_STATUS: [2:0] busy/done/error, [15:8] terminal status code when available. TRAIN_OBSERVATION0 holds the response observation. | MODEL | VP-BOOT-33 | Bit order and codes: GAP-LC-11. |
| LC-9.1.2-11 | H:L4513 (BE18) | PHY_WRLVL_DELAY[2:0] code c drives a thermometer value of (2^c)−1 (0→0x00, 3→0x07, 7→0x7F). | MODEL | VP-BOOT-34 | Check this value at the provider. |
| LC-9.1.2-12 | H:L4509–4515 | PHY_RDLVL/GATE use [23:0]; [31:24] are reserved write-zero. PHY_SKEW_CTRL0/1 are opaque 32-bit words read back unchanged. | MODEL | VP-CSR-41 | |
| LC-9.1.2-13 | H:L3018, H:L5341 (BE10) | Load strobes do not authorize in-place retraining. Live update is disabled. | MODEL (negative) | VP-BOOT-35 | |
| LC-9.1.2-14 | H:L4526 | PHY_BRIDGE_ERROR[7:0] is a sticky bridge/CDC/load error code (W1C). | APPROX | VP-BOOT-36 | Code/bit map: GAP-LC-16. |

### 2.10 Readiness, maintenance, scrub (LC-9.1 / LC-9.4)

| Req ID | Source | Requirement | VP class | Proposed VP test ID | Notes |
|---|---|---|---|---|---|
| LC-9.1-01 | H:L5049–5053, H:L5182–5190, H:L5569–5573 | mission_ready = phy_ready_requirement ∧ init_done ∧ ¬training_busy ∧ training_complete ∧ scrub_complete ∧ power_allows ∧ ¬fatal ∧ ¬recovery_required. | MODEL | VP-LIFE-02 | Drop each term in turn and check that mission_ready falls. |
| LC-9.4-01 | H:L5567 | admission_block = ¬mission_ready ∨ software_stop ∨ fatal. | MODEL | VP-LIFE-05 | |
| LC-9.4-02 | M:L3297–3312 | Admission terms also include GLOBAL_CTRL.enable and image validity. A non-fatal stop closes admission only together with fe_drained. Fatal terms close it at once (MAS-adopted). | APPROX | VP-LIFE-07 | |
| LC-9.4-03 | H:L5605–5607 | Mission-gate status levels are published: mission-ready, admission-blocked, init-complete, training-ready, scrub-complete, power-allowed, fatal, recovery-required. | APPROX | VP-LIFE-08 | Mapping onto CSR bits is undefined (snapshot composition GAP-CSR-20). |
| LC-9.1-02 | H:L5029, H:L5090–5098 | Maintenance priority: 1 init/power-exit recovery, 2 mandatory refresh, 3 active training, 4 power entry, 5 high/normal/background refresh. Equal classes rotate. | APPROX | VP-LIFE-09 | The VP needs only class ordering, not the arbitration microarchitecture. |
| LC-9.1-03 | H:L5023, H:L5073–5077, H:L5137 (BE03), M:L5255 | With ECC, scrub issues N_total = U + U/8 zero writes with full masks. Only accepted writes advance it. mission_ready waits for completion. Readiness loss restarts from granule 0. Explicit failure latches scrub fatal. A PD/SR round trip does not count as readiness loss. | APPROX | VP-SCRUB-01 | Write-count check against committed U. |
| LC-9.1-04 | H:L5067 | With ECC disabled, scrub_complete is true. | MODEL | VP-SCRUB-02 | |
| LC-9.4-04 | H:L5555–5559 | Status distinguishes activity, readiness, failure, timeout and recovery-required. Invalid geometry is reported as unavailable/faulted, not as ECC disabled. | APPROX | VP-LIFE-08 | |

### 2.11 Refresh (LC-9.2)

| Req ID | Source | Requirement | VP class | Proposed VP test ID | Notes |
|---|---|---|---|---|---|
| LC-9.2-01 | H:L5365–5367, H:L5386–5388 | Per active rank: d_next = clamp(d + missed_interval − refresh_credit, −L, +L). next_deadline = previous + tREFI (core cycles). Credit counts only after the accepted REF's tRFC has passed. | MODEL | VP-LIFE-40 | |
| LC-9.2-02 | H:L5448 (BE04) | For limit N: H = (N+1)>>1. Mandatory at d ≥ N. High for H ≤ d ≤ N−1. Normal for 1 ≤ d ≤ H−1. Background pull-in for d ≤ 0, never below −N. Example: N=5 gives high at 3–4 and mandatory at 5. | MODEL | VP-LIFE-41 | Sweep N = 1..8. N=1 or 2 leaves the normal band empty, and N=1 leaves high empty. |
| LC-9.2-03 | H:L5353, H:L5410 | Mandatory urgency blocks new issue to that rank until REF is accepted and tRFC has passed. Other ranks continue only while shared timing stays safe. | MODEL | VP-LIFE-42 | |
| LC-9.2-04 | H:L5355, H:L5419 | If a bank is open, the scheduler issues PREA first, then REF. | APPROX | VP-LIFE-43 | |
| LC-9.2-05 | H:L5359, H:L5411 | Debt saturation or illegal timing is reported as a fault without wrapping the debt. | MODEL | VP-LIFE-44 | Saturation definition and IRQ_RAW[5] trigger: GAP-LC-18. |
| LC-9.2-06 | H:L5357 | Self-refresh holds ordinary debt. A full refresh interval restarts after a legal exit. | MODEL | VP-POWER-20 | |
| LC-9.2-07 | H:L5431 | A rejected refresh proposal leaves debt unchanged. | APPROX | VP-LIFE-45 | |
| LC-9.2-08 | H:L4418–4422, H:L5438 | REFRESH_CFG[3:0] (reset 8, legal 1..8). REFRESH_RANK0/1: [4:0] signed debt, [6:5] urgency. REFRESH_STATUS[4:0]: derived flags. All snapshot-backed. | MODEL | VP-LIFE-46 | Urgency and REFRESH_STATUS encodings: GAP-LC-17. |
| LC-9.2-09 | H:L5648 | A scheduler/service policy update does not by itself reinitialize DRAM. A tREFI update restarts refresh interval accounting. | MODEL | VP-LIFE-47 | tREFI is in the timing bank, so per DEC-05 its commit also triggers reinit. |

### 2.12 Power lifecycle (LC-9.3)

| Req ID | Source | Requirement | VP class | Proposed VP test ID | Notes |
|---|---|---|---|---|---|
| LC-9.3-01 | H:L5458, H:L5514 | A power request is checked against lifecycle ownership, refresh urgency, protected updates, bank legality and the command mutex before any irreversible step. An unsupported or busy request is rejected (PDENY). | MODEL | VP-POWER-03 | |
| LC-9.3-02 | H:L5460, H:L5493–5495 | Entry stops admission and waits for drained = frontend_empty ∧ backend_empty ∧ dfi_idle ∧ ¬rmw_busy ∧ ¬maintenance_conflict. Any required PREA/refresh finishes before entry. | MODEL | VP-POWER-04 | |
| LC-9.3-03 | H:L5464, H:L5472–5474 | deadline = transition_start + POWER_TIMEOUT. A timeout before the irreversible CKE/entry point denies the request and returns to active. A timeout after it sets recovery-required (POWER_STATUS[5]), holds admission closed, and allows only the authorized recovery. | MODEL | VP-POWER-05 | Recovery path: GAP-LC-20. |
| LC-9.3-04 | H:L5512–5519 | State sequence: Active → Quiesce → Prepare → Enter → Resident → Wake/exit. Admission stays closed until wake is ready. | MODEL | VP-POWER-06 | |
| LC-9.3-05 | H:L5547 (RS12) | Config and saved training context are retained while the controller is powered (no retention through power removal). Wake waits for memory exit timing and PHY ready. Cold reset needs full init. | MODEL | VP-POWER-07 | No retraining on wake. |
| LC-9.3-06 | H:L5462, H:L5543 (RD-PWR-05) | Supported intents: APD, PPD, and SR of all active ranks. Partial-rank SR is excluded. | MODEL | VP-POWER-08 | |
| LC-9.3-07 | M:L1162 | A direct power-down ↔ self-refresh change is denied, so the power controller must request active first (MAS-adopted). | MODEL | VP-POWER-09 | |
| LC-9.3-08 | M:L4501, M:L4190 | PDENY to any entry request while the topology-mismatch commit block is set (MAS-adopted). Refresh continues. | APPROX | VP-POWER-11 | |
| LC-9.3-09 | H:L4458–4466, H:L4535 | POWER_STATUS: [1:0] resident, [2] active, [3] accept, [4] deny, [5] fatal recovery (snapshot-backed). POWER_ERROR[23:0] is a sticky W1C code. POWER_TIMEOUT reset is 1024 core cycles and must be nonzero. A failure sets IRQ_RAW[8]. | MODEL | VP-POWER-12 | POWER_ERROR bits: GAP-LC-19. |
| LC-9.3-10 | H:L5528 | Request and response stay serialized by the command mutex. | APPROX | VP-POWER-13 | With POWER_CMD rejected, the mutex sees only P-channel requests (deny on busy, proposal). |
| LC-9.3-11 | H:L5021, M:L3305, M:L3312 | power_allows is the readiness term for the resident power state. | APPROX | VP-POWER-14 | MAS keeps admission open in power-down (C-16): GAP-LC-21. |
| LC-9.3-12 | M:L793, M:L1312 | PACTIVE shows when the controller needs the active state. Its reset value is 1. | APPROX | VP-POWER-15 | Exact assertion terms: GAP-LC-22. |

---

## 3. Snapshot-backed vs live registers (HAS Table 8-5)

Classes:
- **K**: elaborated constant.
- **L**: local CSR storage, live.
- **S**: shadow storage.
- **A**: acknowledged active mirror (CSR-side, live after install).
- **R**: command-result mailbox (updated at each endpoint result).
- **SNAP**: coherent snapshot. Reads 0 until the first valid capture and stays stable between captures.
- **STK**: live sticky W1C event.
- **LV**: live level.
- **Z0**: reads 0 by decision or profile.

| Register (offset) | Class | Basis |
|---|---|---|
| IP_ID 0x0000, ABI_VERSION 0x0004, CAP_DEPTH0 0x000C, CAP_WIDTH0 0x0010 | K | H:L4249 |
| CAP_FEATURE 0x0008 | K | H:L4563 "populated from the CSR capability image" |
| GLOBAL_CTRL 0x0014, SCRATCH 0x0034 | L | H:L4276 |
| GLOBAL_STATUS 0x0018 | Z0 | H:L4112 "0x00000000 in this profile" |
| QUIESCE_CTRL 0x001C, SNAPSHOT_CMD 0x0028, FE_PORT_CMD 0x0040, FE_REGION_COMMIT 0x0180, TOPO_COMMIT 0x1020, TIMING_COMMIT 0x1090, INIT_CMD 0x3040, POWER_CMD 0x30C0, DFI_TIMING_COMMIT 0x5020, TRAIN_CMD 0x5080, IRQ_TEST_SET 0x6010 | WO-CMD, read 0 | H:L4092 |
| QUIESCE_STATUS 0x0020 | LV (proposal) | H:L4263–4269 call the fields "input" levels. H:L4276 routes the result/status path through the command handshake, so the class is ambiguous (GAP-CSR-13). |
| RESET_CAUSE 0x0024 | STK, Z0 in profile | H:L4317 |
| SNAPSHOT_STATUS 0x002C | L | H:L4272–4274 |
| COMMAND_ERROR 0x0030 | STK | H:L4276 |
| FE_PORT_CMD_STATUS 0x0044 | R | H:L4302 |
| FE_PORT_STATE 0x0048 | R (published on successful result) | H:L4302 |
| FE_REGION_SHADOW[0..31] 0x0100–0x017C | S | H:L4123 |
| FE_REGION_STATUS 0x0184 | R | H:L4328–4332 |
| FE_REGION_ACTIVE[0..31] 0x0200–0x027C | A | H:L4326 |
| TOPO_SHADOW 0x1000, T_RCD..PHY_READ_LATENCY 0x1040–0x1088 | S | H:L4364 |
| TOPO_ACTIVE 0x1004, A_T_RCD..A_PHY_READ_LATENCY 0x1100–0x1148 | A | H:L4356, H:L4364 |
| USABLE_GRANULES_LO/HI, PHYSICAL_GRANULES_LO/HI, ECC_BASE_LO/HI 0x1008–0x101C | SNAP | H:L4390 (RS08) "same snapshot" |
| TOPO_COMMIT_STATUS 0x1024, TIMING_COMMIT_STATUS 0x1094, DFI_TIMING_STATUS 0x5024 | R | H:L4350–4362, H:L4505–4508 |
| WRITE_DRAIN_CFG 0x2000, STARVE_LIMIT 0x2004, READ_TIMEOUT 0x2040 | L (live policy image) | H:L4410 |
| SCHED_STATUS 0x2008, DISPATCH_STATUS 0x2044, READ_META_STATUS 0x2048 | SNAP by RS09, value Z0 (no bit layout) | H:L4410, H:L4563, decision |
| STAGED_WRITE_OCC 0x200C | SNAP | H:L4404 "acknowledged backend occupancy snapshot word" |
| WAIT_OLDEST_BE/SRT/HRT 0x2010–0x2018, SERVICE_COUNT_BE/SRT/HRT 0x2020–0x2028 | SNAP (proposal) | RS09 lists only "scheduler candidate/held-valid state". These counters are covered only by H:L4096 "dynamic status… coherent snapshot data". |
| REFRESH_CFG 0x3000 | L | H:L4047 live policy |
| REFRESH_RANK0/1 0x3004/0x3008, REFRESH_STATUS 0x300C | SNAP | H:L4468, H:L4563 |
| INIT_STATUS 0x3044 | SNAP | H:L4468 |
| INIT_ERROR 0x3048, POWER_ERROR 0x30D4, PHY_BRIDGE_ERROR 0x509C | STK | H:L4276, H:L4446, H:L4468 "mailbox/W1C paths" |
| MR0..MR3, INIT_* waits, INIT_CMD_TIMEOUT, TRAIN_TRAS_WAIT, T_CKSRE, T_CKSRX, PHY_RDLVL/GATE/WRLVL/SKEW_CTRL0/1 | L (INIT_RW, lockable) | H:L4094, H:L4531 |
| POWER_STATUS 0x30C4 | SNAP | H:L4468, H:L4563 |
| POWER_TIMEOUT 0x30C8, CE_THRESHOLD 0x4004, TRAIN_TIMEOUT 0x5088, IRQ_MASK 0x6004 | L | H:L4047 |
| ECC_CAP_STATUS 0x4000 | Mixed: [0], [12:9], [20:13] K; [1], [8:2] SNAP | H:L4478–4482 |
| CE_COUNT, UE_COUNT, ECC_LAST_ADDR_LO/HI, ECC_LAST_INFO 0x4008–0x4018 | SNAP; Z0 without ECC | H:L4492, H:L4563 |
| QUARANTINE_STATUS, QUARANTINE_FIRST_REQ_LO/HI 0x4040–0x4048 | Latched (RS10), read path profile-defined. Proposal: LV of the latched record | H:L4492, H:L4563 |
| FATAL_STATUS 0x4050 | STK, HW-latched, no SW clear (RO) | H:L4492 |
| BACKEND_OCCUPANCY0/1 0x4054/0x4058 | SNAP, value Z0 (no layout) | H:L4563, decision |
| PHY_CAP 0x5000 | K | H:L4192 |
| PHY_STATUS 0x5004 | SNAP (BE18 "captures… coherently"), proposal | H:L4500, H:L4533 |
| DFI_WR/RD_ENABLE_DELAY, PHY_MAX_READ_RETURN, DFI_ASSEMBLY_MARGIN 0x5010–0x501C | S | H:L4529 |
| TRAIN_STATUS 0x5084, TRAIN_OBSERVATION0 0x508C | SNAP | H:L4563 "training… observations" |
| PHY_COMP_NCTL/PCTL/RXCTL 0x5090–0x5098 | SNAP (BE18), profile-defined | H:L4533, H:L4563 |
| IRQ_RAW 0x6000, IRQ_STATUS 0x6008 | LV (sticky raw / combinational) | H:L4559 |
| IRQ_CLEAR 0x600C | W1C command, no storage | H:L4216 |

The words marked SNAP outnumber the 30-word bank. Counting RS08 (6), RS09 (10), refresh (3), init (1), power (1), occupancy (2), training (2), PHY status/comp (4) and quarantine (3) gives 32 or more, before ECC. H:L4070 places the ECC leaves outside the 30 words. The VP therefore captures every SNAP register atomically and does not commit to a 30-word layout (GAP-CSR-20).

---

## 4. HAS vs MAS conflicts

| ID | Topic | HAS | MAS | Resolution |
|---|---|---|---|---|
| C-01 | TOPO_SHADOW reset | 0x00000000 (H:L4127, L4364) | 0x501 x8 / 0x6F1 x4 (M:L1898) | HAS (SPEC-03) |
| C-02 | Timing shadow reset | 0 (H:L4137, L4364) | speed-bin image (M:L1743–1771) | HAS (SPEC-03) |
| C-03 | INIT waits / TRAIN_TRAS_WAIT reset | 1 / 0x14 (H:L4165–4172) | speed-bin tCK / 28 (M:L1775–1788, L1910) | HAS |
| C-04 | DFI_WR/RD_ENABLE_DELAY reset | 0 (H:L4194–4195) | 4 / 13 (M:L1824–1825) | HAS |
| C-05 | "Reset image passes commit; first commit accepted with no write" | Not claimed. RS11: no unchecked bootstrap timing (H:L5242) | Claimed (M:L1741, L1919; M:L4849 DFI-RS-01) | HAS. Under HAS resets, TIMING_COMMIT before programming fails validation. |
| C-06 | T_CKSRE/T_CKSRX at 0x30CC/0x30D0 | INIT_RW, reset 1 (H:L4176–4177) | Reserved RAZ/WI; the real ones sit in the timing bank (M:L1792) | HAS (MAS timing-bank copies are DEC-08 build params) |
| C-07 | INIT_CMD_TIMEOUT | "not an enforced watchdog" (H:L4455) | "is a watchdog" (M:L1790) | HAS |
| C-08 | INIT wait units | configured_wait in core cycles (H:L5170). PHY reset is 4 core cycles (H:L4470). | tCK, converted ceil(n/R) (M:L1472, L1737, L1909) | HAS wording wins, but RS11 is ambiguous: GAP-LC-04 |
| C-09 | CE_THRESHOLD = 0 | "nonzero" (H:L4483) | 0 = off (M:L1913) | HAS: writing 0 returns SLVERR |
| C-10 | Live DFI update | Excluded (BE10/BE17, CAP_FEATURE[9]=0) | phyupd ack within 25 clocks, ctrlupd before SRX (M:L856–857, L1035–1036) | HAS: OUT |
| C-11 | Frequency change | Not in profile. Timing changes go through stop/drain/reinit (H:L730) | Supported, FREQ_CHANGE_CMD (M:L858, L1039, L1503). MAS also contradicts itself (M:L858 vs L1150). | HAS + DEC-08: OUT |
| C-12 | init_complete ownership | Controller output, no PHY input (H:L3042) | dfi_init_complete from PHY, dfi_init_start (M:L857, L1503) | HAS |
| C-13 | PHY boundary | Normalized BE07 levels + BE12 bundles | DFI 4.0 pins + i_phy_reset_n (M:L1203, L1293) | HAS (VP abstracts to the functional provider) |
| C-14 | QoS / reserved-rate config | Sideband (H:L723–724) | CSR only, "no sideband configuration port" (M:L775, L1058, L1100, L1917) | HAS + DEC-04/DEC-08 |
| C-15 | Image sizes | Param 18×32, timing 19×32, DFI 4×32 (H:L4068) | Param 30 words, timing 31 + dll_off_pd, DFI 12 (M:L1382–1383) | HAS + DEC-08 |
| C-16 | Admission in power-down | Entry stops admission and drains for every intent. Exit keeps admission closed until ready (H:L5021, L5460, L5519) | "A power-down raises no stop", and mission_ready "stays 1 in power-down" (M:L3305, L3312) | HAS: GAP-LC-21 |
| C-17 | Extra mission_ready terms | HAS formula (H:L5049) | Adds no rank in SR, no freq change, ¬scrub fatal, ¬power fatal, ¬init failed (M:L5252) | Compatible. Adopt the MAS terms as refinements of power_allows, scrub_complete and init_ready (marked MAS). |
| C-18 | GLOBAL_CTRL.enable semantics | "controller enable", no semantics (H:L4257) | Admission term ctl_mission_enable (M:L3304) | HAS silent → adopt MAS |
| C-19 | Region-commit capacity check and codes | No capacity check or codes (H:L4334) | 0x0B/0x0C/0x0D, end within USABLE_GRANULES (M:L3202, L3214) | HAS silent → adopt MAS |
| C-20 | Commit blocker codes | Locations only | 0x04 timing, 0x11 topology (M:L1869, L1881) | Adopt MAS (marked). Locations follow HAS. |
| C-21 | FE_PORT_CMD ops | op[2:0], no codes | op 1 quiesce, op 3 soft reset (M:L3138–3139) | Adopt MAS |
| C-22 | Port quiesce timeout | Indefinite; timeout status stays 0 (FE10, RD-CSR-02) | FE_QUIESCE_TIMEOUT register, status only (M:L963, L1563, L1892) | HAS. Register not exposed; build param = 0. |
| C-23 | MAS registers missing from the HAS map | Unlisted → DECERR (H:L4088) | ERROR_STATUS 0x404C, BAD_RETURN_LAST, WRITE_FETCH_TIMEOUT 0x2054, SCHED_EVENT_COUNT, FE_QUIESCE_TIMEOUT, READ_GATE_OFFSET, DFI_WRDATA_DELAY, DFI_T* (M:L974, L1596–1597, L1828–1835, L4442, L4497) | HAS: DECERR. Recommend adding these to the DEC-08 list (not all are named there). |
| C-24 | FATAL_STATUS bits | "sticky fatal-source bitmap", no layout (H:L4489) | [3] DFI protocol hold, [4] fetch_timeout, [5] topology_mismatch, [6] ws_protocol_fault, [7] wr_result_unmatched (M:L4493–4496, L4836) | HAS silent → adopt MAS for [7:3]. [2:0] and the rest: GAP-CSR-16 |
| C-25 | QUARANTINE_STATUS layout | "active/reset-required/cause/context" (H:L4487) | [1:0]=2'b11 active, [4:2] reason (0 timeout; 1–4 other), [15:8] open contexts (M:L4514, L4491–4492) | HAS silent → adopt MAS. Reason 1–4 meanings: GAP-CSR-12 |
| C-26 | Quarantine / fatal recovery | Coordinated cold reset (RS10, H:L4492; BE15) | "Only a reset clears them" (M:L1165) | Compatible. VP: cold reset only. |
| C-27 | INIT software retry | Table 9-10 has a "software-authorized retry request" (H:L5223), but INIT_CMD returns SLVERR (H:L4424) | INIT_CMD returns SLVERR, restart after commit (M:L1163) | DEC-05. HAS contradicts itself; Table 9-10 retry is OUT. |
| C-28 | Power request source | Table 9-23 "Control software" (H:L5525), but POWER_CMD returns SLVERR | P-channel only (M:L1163) | DEC-04: P-channel |
| C-29 | Training initiation | "firmware-directed" (BE06, H:L881, L5019) | — | DEC-06: boot automatic, runtime by TRAIN_CMD |
| C-30 | Rank change before first commit | — | Active ranks set via topology before the first timing commit (M:L853, L1501) | Adopt MAS as a software-sequence note (not enforced) |
| C-31 | INIT error forwarding | One-hot code (H:L4446) | Forwarded only if exactly one bit is set (M:L1177) | Compatible: adopt MAS |
| C-32 | MR write validation | None specified | Rtt/drive/SRT-ASR checks (M:L1873) | HAS silent → adopt MAS as INIT_RW validation (SLVERR) |

---

## 5. Gaps and open issues

Each "Proposal" is a proposed VP default for review. None of them is a source fact.

| ID | Gap | Evidence | Proposal |
|---|---|---|---|
| GAP-LC-01 | INIT_STATUS[10:7] stage encoding undefined | H:L4432 gives only "initialization stage". Table 9-9 lists groups, not codes. | 0 HOLD, 1 PHY_RESET, 2 PHY_READY_WAIT, 3 RESET_RELEASE_WAIT, 4 CKE_LOW_WAIT, 5 CKE_HIGH_WAIT, 6 MR2, 7 MR3, 8 MR1, 9 MR0, 0xA DLL_WAIT, 0xB ZQCL, 0xC ZQ_WAIT, 0xD TRAINING, 0xE DONE, 0xF FAILED. |
| GAP-LC-02 | INIT_ERROR[15:0] one-hot bit map undefined | H:L4446 | Bit n = stage code n (GAP-LC-01) at the moment of failure. Out-of-range rank count → bit 0 (HOLD). PHY failure → the stage in progress. Boot training fail → bit 13. |
| GAP-LC-03 | BE05 restart triggers are broader than DEC-05 (MR, ratio, PHY profile, PHY clock/reset/training changes) | H:L879, L5145, L5648 | Only TOPO, TIMING and DFI commits restart init. MR/INIT_RW changes take effect at the next restart. A ratio or PHY-profile change is build-time, so OUT. |
| GAP-LC-04 | INIT wait units (core cycles vs tCK). Whether DRAM RESET# assert time is programmable. The 14-entry wait image with only 6 used. | H:L4470, L5170, L5234 vs M:L1737 | Treat INIT_* waits as core cycles (HAS). DRAM RESET# low time is ABSTRACTED (the provider owns it). The 8 unused wait entries are not modeled. |
| GAP-LC-05 | Which transition timing_wait_ok gates | H:L726 "gates the corresponding timed transition" | Gate each init wait-stage exit (stage_done ∧ timing_wait_ok). Provider default is 1. |
| GAP-LC-06 | "Initialization lock" point for INIT_RW undefined | H:L4094, L4531 | Locked from leaving HOLD until init reaches DONE or FAILED, and while a runtime TRAIN_CMD is active. Writes while locked return SLVERR and set COMMAND_ERROR[2]. Values are captured at stage-1 entry. |
| GAP-LC-07 | Quiesce prerequisite for TOPO/TIMING/DFI commits at boot | H:L879 BE05 "requires global quiescence"; M:L1868 | quiesce_ok = (QUIESCE_STATUS.complete) ∨ (mission admission has not opened since the last reset/reinit ∧ global idle). The boot-time commit then needs no QUIESCE_CTRL. |
| GAP-LC-08 | QUIESCE_CTRL semantics: both or no bits set, B-response timing, mutex hold under the FE10 indefinite wait | H:L4260–4262, L4276, L2077 | Exactly one of [1:0] must be set, otherwise SLVERR. B returns once the request/release is applied, not at drain. The mutex is released at that point. Completion is polled through QUIESCE_STATUS[5]. A release without an outstanding request returns OKAY as a no-op. |
| GAP-LC-09 | SKEW_CTRL0/1 transport: only 3 load strobes exist | H:L715, L4515, L3039 | Pass SKEW words as opaque attributes of the same rank/epoch/tag provider transaction (no 4th load). |
| GAP-LC-10 | Boot-training timeout source | H:L5270 (command_timeout). TRAIN_TIMEOUT is described for commands. | Use TRAIN_TIMEOUT (reset 1024 core cycles) per load. Timeout → INIT_ERROR bit 13 and IRQ_RAW[6]. Whether IRQ_RAW[7] is also set is open. Proposal: set both. |
| GAP-LC-11 | TRAIN_CMD primitive codes, the [11:4] rank/lane layout, [27:12] options, TRAIN_STATUS[2:0] bit order, [15:8] codes | H:L4516–4521 | Primitives: 0 LOAD_RDLVL, 1 LOAD_GATE, 2 LOAD_WRLVL, 3 LOAD_ALL, 4–7 reserved (SLVERR). [4] rank, [8:5] byte-lane mask, [11:9] reserved-zero. Options are opaque to the provider. TRAIN_STATUS: [0] busy, [1] done, [2] error. [15:8]: 0 OK, 1 timeout, 2 stale, 3 PHY fail, 4 rejected (precondition). |
| GAP-LC-12 | Runtime training: does the controller perform admission stop, drain and LP exit, or only check them? Does "reinitialization path" mean full DRAM reinit? | H:L5254 vs L3058/L5326 | The controller stops admission and drains internally. If the power state is not ACTIVE, it rejects immediately (SLVERR, code 4), because only the P-channel can exit LP. No MRS/ZQ rerun. On success admission reopens. On failure training_error stays sticky and mission_ready stays 0 until the next reinit commit or reset. |
| GAP-LC-13 | RESET_CAUSE "reads zero in this profile" vs the RD-RST-02 codes | H:L4310, L4317 | Read 0 by default. A VP parameter `populate_reset_cause` (default false) enables the RD-RST-02 codes. |
| GAP-LC-14 | RD-RST-01 reset-ack timeout and recovery values absent | H:L2946 | No timeout. reset_safe waits indefinitely, matching FE10. |
| GAP-LC-15 | DEC-04 does not cover port quiesce/soft-reset pins or the core/CSR reset handshake drivers | H:L709, M:L1201 | DDR_SYSCTRL ties external port requests to 0 and exposes test hooks. Core/CSR reset_req come from the platform reset controller model. |
| GAP-LC-16 | PHY_BRIDGE_ERROR[7:0] codes | H:L4526 | Bitmap: [0] load timeout, [1] epoch/tag stale, [2] provider failure, [3] load while PHY not ready, [7:4] reserved. |
| GAP-LC-17 | REFRESH_RANK urgency[6:5] encoding (names 3 levels; BE04 has 4) and the REFRESH_STATUS[4:0] bit order | H:L4420, L4422, L5448 | Urgency: 0 background, 1 normal, 2 high, 3 mandatory. REFRESH_STATUS: [1:0] per-rank mandatory, [2] any high-or-mandatory, [3] any mandatory, [4] saturation. |
| GAP-LC-18 | Definition of refresh "saturation" when debt is clamped at +L. Trigger of IRQ_RAW[5]. | H:L5365, L5411 | Saturation = a missed interval while d = +L (the increment is lost). It sets REFRESH_STATUS[4] and IRQ_RAW[5]. |
| GAP-LC-19 | POWER_ERROR[23:0] codes | H:L4465 | Bitmap: [0] denied (busy/mutex), [1] denied (illegal transition, including PD↔SR), [2] pre-cutoff timeout, [3] post-cutoff timeout, [4] wake PHY-not-ready, [5] denied (commit block). |
| GAP-LC-20 | "Authorized recovery sequence" after a post-cutoff power timeout | H:L5464 | Only a core/cold reset clears POWER_STATUS[5]. A TOPO/TIMING/DFI commit is rejected while recovery_required (proposed blocker). |
| GAP-LC-21 | power_allows for resident APD/PPD (C-16) | H:L5021, L5063 vs M:L3305 | power_allows = resident ACTIVE ∧ no transition in progress. Traffic while in PD waits for the P-channel wake. |
| GAP-LC-22 | PACTIVE assertion terms. Refresh obligations while resident in APD/PPD. | M:L793, L1312; H:L5357 | PACTIVE = AXI work pending at any port ∨ refresh urgency ≥ high on a rank in APD/PPD. Reset value 1. DDR_SYSCTRL policy decides on wake. |
| GAP-LC-23 | P-channel request to the current state, and request while the mutex is held by a CSR command | H:L5528 | Same state: accept (no-op). Mutex busy: PDENY and POWER_ERROR[0]. |
| GAP-CSR-01 | Precedence for a misaligned access to an unmapped word | H:L4569 vs Table 8-21 | Alignment first → SLVERR. Full order: alignment → mapping (DECERR) → access class → strobe → reserved bits → mutex busy → interlock/validation. |
| GAP-CSR-02 | IRQ_CLEAR read value | H:L4216 "no storage" | Reads 0 with OKAY. |
| GAP-CSR-03 | Reserved-bit SLVERR vs ignore | H:L4092, L4571 | "write zero" fields return SLVERR. "Writes ignored/no effect" and "read zero" only (IRQ_*[31:12]) fields are ignored. |
| GAP-CSR-04 | "Error classification is side-effect free" vs sticky COMMAND_ERROR set paths | H:L4575 vs L4312–4317 | No functional side effect on the target. COMMAND_ERROR[0] is set on mutex/snapshot busy, [1] on endpoint-result failure, [2] on commit validation/interlock rejection. Each also sets IRQ_RAW[10]. Pure decode errors (DECERR, strobe, RO, INIT_CMD/POWER_CMD) set nothing. |
| GAP-CSR-05 | Response for an acknowledged-completion failure, and whether a new commit clears old blockers | H:L4385–4390 | Return SLVERR. A new accepted commit clears done, error and both blocker fields first (same as FE_PORT). |
| GAP-CSR-06 | Full blocker code table: meaning of 0x04/0x11, codes for interlock (not quiesced, not idle), DFI, FE_PORT | M:L1881 "Chapter 8 lists all of them" (missing) | Keep 0x04, 0x11, 0x0B–0x0D. Allocate VP-local codes 0xE0–0xEF (not quiesced, mutex, not idle, recovery required, field illegal) and mark them provisional. |
| GAP-CSR-07 | Set/clear rule for TOPO/TIMING_COMMIT_STATUS[2] reinit required (DFI status has none) | H:L4352, L4360 | Set on each successful commit that triggers reinit. Cleared when init reaches DONE or on the next commit. |
| GAP-CSR-08 | Is FE_REGION_COMMIT a mutex owner? | H:L4766, Table 8-29 do not list it | Yes. It is serialized and returns SLVERR on busy. |
| GAP-CSR-09 | SNAPSHOT_CMD B timing and the first sequence value | H:L4270–4274 | B returns after capture_ack (OKAY). The sequence starts at 0 after reset and the first capture publishes 1. |
| GAP-CSR-10 | Exact triggers for IRQ_RAW[4] mapping/config, [5], [9], [10] | H:L4545–4551 | [4]: commit validation reject or AXI decode-region error. [5]: refresh saturation. [9]: PHY failure rising edge. [10]: per GAP-CSR-04. |
| GAP-CSR-11 | FE_PORT_CMD op codes 0/2/4–7, blocker codes, soft reset without ack | M:L3138–3142, L3165 | 0 reserved, 1 quiesce, 2 release quiesce, 3 soft reset, 4–7 reserved (SLVERR). Soft reset without ack: immediate SLVERR, blocker 0xE1. Port ≥ N_PORT: SLVERR. |
| GAP-CSR-12 | QUARANTINE_STATUS reasons 1–4, reset-required bit, [31:16] | H:L4487; M:L4491–4498 (3 and 4 are stale-related) | 1 malformed shape, 2 unexpected/extra, 3 stale epoch, 4 tokenless. Reset-required equals active. Other bits 0. |
| GAP-CSR-13 | QUIESCE_STATUS read class | H:L4263–4276 | Live level. |
| GAP-CSR-14 | DEC-07 vs HAS reset readback of FE_REGION_ACTIVE (0x0) and FE_REGION_STATUS[16] (0). Fallback after core offline. Bootstrap validation. | H:L4126, L4334; M:L3192 | Decode uses the bootstrap map. FE_REGION_ACTIVE reads 0 and [16]=0 until the first successful commit (SPEC-03 readback). Core offline reverts decode to bootstrap. Bootstrap is not validated. |
| GAP-CSR-15 | CAP_FEATURE[8:7] map-profile mask value, and the full CAP_FEATURE value | H:L4236; M:L1580 (MAP_PROFILE_MASK = 3, bit 0 linear, bit 1 interleaved) | Default native R2 with ECC, both maps and IRQ test: CAP_FEATURE = 0x00000DD5 (= ECC 1, ratio 2, ranks 2, PHY ranks 2, mask 3, ratio-2 1, IRQ test 1). It is a derived proposal. Also TOPO[12]: 0 = linear, 1 = interleaved (proposal). |
| GAP-CSR-16 | FATAL_STATUS[2:0] and [31:8] | H:L4489; M:L4493–4496 | [0] ECC fatal hold, [1] scrub fatal, [2] request sequence fault (from MAS admission terms M:L3308–3309). Others 0. |
| GAP-CSR-17 | ECC_LAST_INFO layout | H:L4486 | [1:0] type (1 CE, 2 UE), [3:2] codeword 0..3, [15:8] syndrome, other bits 0. Defer to the ECC audit. |
| GAP-CSR-18 | PHY_CAP / PHY_STATUS layout | H:L4500 | PHY_STATUS: [0] reset_done, [1] ready, [2] failure, [3] timing_wait_ok, [5:4] update observation, other bits 0. PHY_CAP is a VP parameter, default 0 (unavailable). |
| GAP-CSR-19 | ECC_CAP_STATUS constant value; behavior with ECC off | H:L4179, L4478–4482 | ECC on and idle: 0x00010801 ([0]=1, [12:9]=4, [20:13]=8). ECC off: 0x00000000 (H:L4096 "Optional ECC words read zero"). |
| GAP-CSR-20 | Composition of the 30-word snapshot | H:L3109, L4070, L4995 | Do not model a 30-word bank. Capture all SNAP registers in §3 atomically under one sequence. |
| GAP-CSR-21 | Coherent-policy write with partial strobes | H:L4092, L4410 | Merge, then validate the merged word. On failure return SLVERR and keep the image. |

---

## 6. Appendix B review items and proposed defaults affecting the VP

Appendix B (H:L5683–L5696) records 4 comments from one reviewer, and each has a response. None is marked open. Each response, however, depends on rules still labeled "Proposed".

| No. | Topic | Status | VP impact |
|---|---|---|---|
| 1 | Bank-state count and rank profile; BE12 rank → chip-select mapping | Dispositioned; BE12 still proposed | Rank index = CS index in provider calls (IF-7.2-04 ABSTRACTED) |
| 2 | Granule packing/reassembly; AXI128/AXI64 width math | Dispositioned | Data-path audit; IF-7.2-01 |
| 3 | Native two-rank ratio-2 architecture; BE12 phase mapping, BE09 rank-qualified training | Dispositioned; BE09/BE12 still proposed | LC-9.1.2-01..05 depend on BE09 identity (rank + epoch16 + tag4) |
| 4 | AXI_DATA_WIDTH = DFI_DATA_WIDTH (AXI64/ratio1, AXI128/ratio2) | Dispositioned | IF-1.5-03, IF-7.2-01 elaboration check |

The VP adopts these "Proposed" HAS rules as if approved. Any review change to them invalidates the listed tests:
- BE04 → VP-LIFE-41
- BE05 → VP-BOOT-10/23
- BE07 → VP-BOOT-02/03
- BE08 → VP-BOOT-01
- BE09 → VP-BOOT-05..09
- BE10 / BE17 → VP-CFG-02
- BE13 → VP-CFG-04
- BE18 → VP-BOOT-34
- RS06 → VP-POWER-10
- RS08 → VP-CFG-41
- RS09 / RS10 → §3
- RS11 → VP-BOOT-12
- RS12 → VP-POWER-07
- FE10 → VP-LIFE-22
- RD-CSR-02 → VP-LIFE-22
- RD-PWR-01 → VP-POWER-01
- RD-PWR-05 → VP-POWER-08
- RD-RST-01 → GAP-LC-14
- RD-RST-02 → GAP-LC-13
