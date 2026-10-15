# DDR3 Controller VP — CSR Register Inventory

Baseline: HAS v1.0 §8.1 Table 8-5 (address summary) and Tables 8-6 to 8-18 (fields),
§8.2 (decode policy), §8.4 (mutex/snapshot), §8.5 (interrupts).
Document hashes are pinned in [baseline.md](baseline.md). Decisions referenced as
`SPEC-xx` / `DEC-xx` are in [decision_log.md](decision_log.md).

This inventory is transcribed by hand from HAS. It is the expectation source for the
CSR tests. The C header and the model register table are written separately and are
checked against this file, not generated from it.

## 1. Global rules (HAS §8.1 "Shared access and update rules", §8.2)

| Rule | Behaviour | Source |
|---|---|---|
| Address space | 16-bit byte address, 32-bit data, 4 strobes; 7 regions of 4 KiB; 210 decoded words | §8.1, §8.2 |
| Alignment | `addr % 4 != 0` → SLVERR, no side effect | §8.2 Table 8-21, example 0x0102 |
| Unmapped / reserved word | DECERR, read data 0, no side effect | §8.1, Table 8-21 |
| RO write | SLVERR, register unchanged | §8.1 example, §8.2 |
| RW / SHADOW / INIT_RW write | merge selected byte lanes; WSTRB=0 is a legal no-op | §8.1 shared rules |
| W1C / WO-CMD write | requires WSTRB=0xF; partial strobes → SLVERR without side effect | §8.1, §8.2 example (W1C WSTRB=0001) |
| WO-CMD read | returns 0 | §8.1 |
| Reserved bits | software writes zero where a field table lists reserved bits; nonzero → SLVERR ("reserved bit" row of Table 8-21) | §8.1, §8.2 |
| Failed validation / interlock | previous value unchanged; SLVERR | §8.1 |
| Busy collision on command mutex | SLVERR, not queued; COMMAND_ERROR[0] set | §8.1, §8.4 |
| Same-cycle read + write to one leaf | read sees pre-write value | §8.2 |
| Hardware set vs W1C | hardware set wins | §8.1, §8.5 |
| Snapshot-backed dynamic words | read 0 before the first acknowledged snapshot (`SNAPSHOT_STATUS.valid=0`) | §8.1 |
| ECC words with ECC disabled | read 0 | §8.1, §8.16 notes |
| Optional feature absent | read 0/OKAY; write SLVERR per leaf contract | Table 8-21 |

VP transport note (not HAS): the CSR TLM target accepts only 4-byte, 4-aligned
transactions; `byte_enable` carries WSTRB (absent = 0xF). Narrower TLM lengths are
answered as misaligned/illegal → SLVERR. See `docs/interfaces.md`.

## 2. Access classes

| Class | Read | Write |
|---|---|---|
| RO | value | SLVERR |
| RW | value | byte-merge; live policy words are range-validated, invalid → SLVERR and keep old value |
| SHADOW | shadow value | byte-merge; takes effect only through the bank commit |
| INIT_RW | value | byte-merge until initialization lock (lock condition: see GAP list in audit) |
| W1C | sticky value | WSTRB must be 0xF; 1 bits clear; hardware set wins |
| WO-CMD | 0 | WSTRB must be 0xF; launches the command (subject to mutex/interlock) |

## 3. Address map (HAS Table 8-5)

Column `Pub` = publication class (HAS Table 8-3 / §8.1 "Status availability"):
L = local, S = shadow/commit, P = live policy, C = command, SN = snapshot-backed,
LV = live level / sticky, K = constant.

### Region 0 — global, frontend (83 words)

| Offset | Name | Access | Reset | Pub | Notes |
|---|---|---|---|---|---|
| 0x0000 | IP_ID | RO | 0x44445233 | K | |
| 0x0004 | ABI_VERSION | RO | 0x00010000 | K | [31:16] major=1, [15:0] minor=0 |
| 0x0008 | CAP_FEATURE | RO | profile constant | K | see §4 |
| 0x000C | CAP_DEPTH0 | RO | 0x04101008 | K | |
| 0x0010 | CAP_WIDTH0 | RO | 0x00208410 (ratio 2) / 0x00204410 (ratio 1) | K | |
| 0x0014 | GLOBAL_CTRL | RW | 0x00000000 | P | [0] enable, [1] admission stop, [31:2] reserved |
| 0x0018 | GLOBAL_STATUS | RO | 0x00000000 | LV | "0x00000000 in this profile"; no fields defined (DEC-04) |
| 0x001C | QUIESCE_CTRL | WO-CMD | — | C | [0] request, [1] release, [31:2] reserved |
| 0x0020 | QUIESCE_STATUS | RO | 0x00000000 | LV | [0] request, [1] adm stopped, [2] global idle, [3] mission idle, [4] DFI tokens empty, [5] complete, [6] timeout (0 when finite timeout disabled, RD-CSR-02) |
| 0x0024 | RESET_CAUSE | W1C | 0x00000000 | LV | [7:0] cause (RD-RST-02 codes); "reads zero in this profile" |
| 0x0028 | SNAPSHOT_CMD | WO-CMD | — | C | [0] capture, [31:1] reserved |
| 0x002C | SNAPSHOT_STATUS | RO | 0x00000000 | LV | [0] busy, [1] valid, [31:16] sequence |
| 0x0030 | COMMAND_ERROR | W1C | 0x00000000 | LV | [0] busy, [1] blocked, [2] commit/interlock |
| 0x0034 | SCRATCH | RW | 0x00000000 | L | byte-mergeable |
| 0x0040 | FE_PORT_CMD | WO-CMD | — | C | [2:0] op, [9:8] port, [31:10],[7:3] reserved |
| 0x0044 | FE_PORT_CMD_STATUS | RO | 0x00000000 | LV | [0] busy, [1] done, [2] error, [5:3] completed op, [9:8] completed port, [23:16] blocker |
| 0x0048 | FE_PORT_STATE | RO | 0x00000000 | LV | [3:0] CSR quiesce, [7:4] eff. quiesce, [11:8] ack, [15:12] reset active, [19:16] timeout, [23:20] reset pending, [27:24] backend drained, [31] valid |
| 0x0100+4i, i=0..31 | FE_REGION_SHADOW[i] | SHADOW | 0x00000000 | S | i = 2*(4p+r)+k; k=0 BASE, k=1 SIZE; SIZE=0 disables |
| 0x0180 | FE_REGION_COMMIT | WO-CMD | — | C | [0] commit |
| 0x0184 | FE_REGION_STATUS | RO | 0x00000000 | LV | [0] busy, [1] done, [2] error, [15:8] blocker, [16] active valid = a committed CSR image is installed (0 while decode uses the bootstrap map) |
| 0x0200+4i, i=0..31 | FE_REGION_ACTIVE[i] | RO | 0x00000000 | S | Committed CSR image only. Reads 0 (and FE_REGION_STATUS[16] reads 0) until the first successful FE_REGION_COMMIT, even though decode uses the bootstrap map meanwhile (DEC-07, HAS §6.1.15 T6-29). Core offline clears it and decode falls back to the bootstrap map |

### Region 1 — topology and timing (50 words)

| Offset | Name | Access | Reset | Pub | Notes |
|---|---|---|---|---|---|
| 0x1000 | TOPO_SHADOW | SHADOW | 0x00000000 | S | [0] ranks-1, [8:4] row bits, [10:9] page code (256/512/1024/2048 words), [12] map profile (linear/interleaved). SPEC-03: HAS reset 0, not MAS 0x501 |
| 0x1004 | TOPO_ACTIVE | RO | 0x00000000 | S | |
| 0x1008 | USABLE_GRANULES_LO | RO | 0 | S | RS08 zero-extended 64-bit |
| 0x100C | USABLE_GRANULES_HI | RO | 0 | S | |
| 0x1010 | PHYSICAL_GRANULES_LO | RO | 0 | S | |
| 0x1014 | PHYSICAL_GRANULES_HI | RO | 0 | S | |
| 0x1018 | ECC_BASE_LO | RO | 0 | S | 0 without ECC |
| 0x101C | ECC_BASE_HI | RO | 0 | S | |
| 0x1020 | TOPO_COMMIT | WO-CMD | — | C | [0] commit |
| 0x1024 | TOPO_COMMIT_STATUS | RO | 0 | LV | [0] done, [1] error, [2] reinit required, [15:8] ack blocker, [23:16] immediate blocker |
| 0x1040+4i, i=0..18 | timing shadow[i] | SHADOW | 0 | S | index table below; unsigned tCK counts |
| 0x1090 | TIMING_COMMIT | WO-CMD | — | C | [0] commit |
| 0x1094 | TIMING_COMMIT_STATUS | RO | 0 | LV | as TOPO_COMMIT_STATUS |
| 0x1100+4i, i=0..18 | timing active[i] (A_*) | RO | 0 | S | |

Timing index (HAS Table 8-12): 0 T_RCD, 1 T_RP, 2 T_RAS, 3 T_RC, 4 T_RRD, 5 T_FAW,
6 T_CCD, 7 T_RTP, 8 T_WR, 9 T_WTR, 10 T_RTW, 11 T_RTRS, 12 T_RFC, 13 T_REFI, 14 CL,
15 CWL, 16 ADDITIVE_LATENCY, 17 PHY_WRITE_LATENCY, 18 PHY_READ_LATENCY.

### Region 2 — scheduler, QoS, dispatch (13 words)

| Offset | Name | Access | Reset | Pub | Notes |
|---|---|---|---|---|---|
| 0x2000 | WRITE_DRAIN_CFG | RW | 0x00100408 | P | [7:0] enter=8, [15:8] exit=4, [31:16] max writes=16 |
| 0x2004 | STARVE_LIMIT | RW | 0x00000400 | P | nonzero |
| 0x2008 | SCHED_STATUS | RO | 0 | SN | DEC-03: unavailable, reads 0 |
| 0x200C | STAGED_WRITE_OCC | RO | 0 | SN | DEC-03 |
| 0x2010 | WAIT_OLDEST_BE | RO | 0 | SN | DEC-03 |
| 0x2014 | WAIT_OLDEST_SRT | RO | 0 | SN | DEC-03 |
| 0x2018 | WAIT_OLDEST_HRT | RO | 0 | SN | DEC-03 |
| 0x2020 | SERVICE_COUNT_BE | RO | 0 | SN | accepted service count (modeled) |
| 0x2024 | SERVICE_COUNT_SRT | RO | 0 | SN | modeled |
| 0x2028 | SERVICE_COUNT_HRT | RO | 0 | SN | modeled |
| 0x2040 | READ_TIMEOUT | RW | 0x00000400 | P | nonzero; validated with policy image |
| 0x2044 | DISPATCH_STATUS | RO | 0 | SN | DEC-03 |
| 0x2048 | READ_META_STATUS | RO | 0 | SN | DEC-03 |

WRITE_DRAIN_CFG, STARVE_LIMIT and READ_TIMEOUT form one validated policy image; an
invalid value keeps the previous image (HAS Table 8-14 note). Range rules: MAS Table 5-20.

### Region 3 — refresh, initialization, power (25 words)

| Offset | Name | Access | Reset | Pub | Notes |
|---|---|---|---|---|---|
| 0x3000 | REFRESH_CFG | RW | 0x00000008 | P | [3:0] active debt limit (1..8) |
| 0x3004 | REFRESH_RANK0 | RO | 0 | SN | [4:0] signed debt, [6:5] urgency |
| 0x3008 | REFRESH_RANK1 | RO | 0 | SN | |
| 0x300C | REFRESH_STATUS | RO | 0 | SN | [4:0] derived status |
| 0x3040 | INIT_CMD | WO-CMD | — | C | [0] start, [1] retry: write → SLVERR, no launch (DEC-05) |
| 0x3044 | INIT_STATUS | RO | 0 | SN | [1] dram initialized, [2] failed, [10:7] stage, [15] admission block; others reserved |
| 0x3048 | INIT_ERROR | W1C | 0 | LV | [15:0] one-hot failing stage |
| 0x3050..0x305C | MR0..MR3 | INIT_RW | 0 | P | payload |
| 0x3060 | INIT_RESET_ASSERT_WAIT | INIT_RW | 1 | P | retained only; fixed 4-core-cycle PHY reset wait |
| 0x3064 | INIT_RESET_RELEASE_WAIT | INIT_RW | 1 | P | |
| 0x3068 | INIT_CKE_LOW_WAIT | INIT_RW | 1 | P | |
| 0x306C | INIT_CKE_HIGH_WAIT | INIT_RW | 1 | P | |
| 0x3070 | INIT_DLL_WAIT | INIT_RW | 1 | P | |
| 0x3074 | INIT_ZQ_WAIT | INIT_RW | 1 | P | |
| 0x3078 | INIT_CMD_TIMEOUT | INIT_RW | 0x00000400 | P | stored only; not an enforced watchdog |
| 0x307C | TRAIN_TRAS_WAIT | INIT_RW | 0x00000014 | P | nonzero |
| 0x30C0 | POWER_CMD | WO-CMD | — | C | write → SLVERR, no launch (power via P-channel) |
| 0x30C4 | POWER_STATUS | RO | 0 | SN | [1:0] resident, [2] active, [3] accept, [4] deny, [5] fatal recovery |
| 0x30C8 | POWER_TIMEOUT | RW | 0x00000400 | P | nonzero |
| 0x30CC | T_CKSRE | INIT_RW | 1 | P | HAS wins over MAS "reserved" (decision log) |
| 0x30D0 | T_CKSRX | INIT_RW | 1 | P | |
| 0x30D4 | POWER_ERROR | W1C | 0 | LV | [23:0] sticky error code |

### Region 4 — ECC, quarantine, backend status (13 words)

| Offset | Name | Access | Reset | Pub | Notes |
|---|---|---|---|---|---|
| 0x4000 | ECC_CAP_STATUS | RO | capability-dependent | SN/K | [0] ECC option, [1] RMW busy, [8:2] lock occupancy, [12:9] codewords=4, [20:13] data bytes=8 |
| 0x4004 | CE_THRESHOLD | RW | 0x00000001 | P | nonzero (HAS); reads 0 without ECC |
| 0x4008 | CE_COUNT | RO | 0 | SN | saturating |
| 0x400C | UE_COUNT | RO | 0 | SN | saturating |
| 0x4010 | ECC_LAST_ADDR_LO | RO | 0 | SN | physical granule |
| 0x4014 | ECC_LAST_ADDR_HI | RO | 0 | SN | |
| 0x4018 | ECC_LAST_INFO | RO | 0 | SN | type/codeword/syndrome |
| 0x4040 | QUARANTINE_STATUS | RO | 0 | LV | RS10 first-record latch |
| 0x4044 | QUARANTINE_FIRST_REQ_LO | RO | 0 | LV | |
| 0x4048 | QUARANTINE_FIRST_REQ_HI | RO | 0 | LV | |
| 0x4050 | FATAL_STATUS | RO | 0 | LV | sticky, no software clear |
| 0x4054 | BACKEND_OCCUPANCY0 | RO | 0 | SN | DEC-03 |
| 0x4058 | BACKEND_OCCUPANCY1 | RO | 0 | SN | DEC-03 |

### Region 5 — DFI, PHY delay, training (21 words)

| Offset | Name | Access | Reset | Pub | Notes |
|---|---|---|---|---|---|
| 0x5000 | PHY_CAP | RO | PHY profile constant | K | |
| 0x5004 | PHY_STATUS | RO | 0 | LV | synchronized status |
| 0x5010 | DFI_WR_ENABLE_DELAY | SHADOW | 0 | S | |
| 0x5014 | DFI_RD_ENABLE_DELAY | SHADOW | 0 | S | |
| 0x5018 | PHY_MAX_READ_RETURN | SHADOW | 0x00000100 | S | |
| 0x501C | DFI_ASSEMBLY_MARGIN | SHADOW | 0x00000001 | S | |
| 0x5020 | DFI_TIMING_COMMIT | WO-CMD | — | C | [0] commit |
| 0x5024 | DFI_TIMING_STATUS | RO | 0 | LV | [0] done, [1] error, [15:8] ack blocker, [23:16] immediate blocker |
| 0x5040 | PHY_RDLVL_DELAY | INIT_RW | 0 | P | [23:0] delay, [31:24] reserved |
| 0x5044 | PHY_GATE_DELAY | INIT_RW | 0 | P | [23:0] delay, [31:24] reserved |
| 0x5048 | PHY_WRLVL_DELAY | INIT_RW | 0 | P | [2:0] code (BE18 thermometer), [31:3] reserved |
| 0x504C | PHY_SKEW_CTRL0 | INIT_RW | 0 | P | opaque provider word |
| 0x5050 | PHY_SKEW_CTRL1 | INIT_RW | 0 | P | opaque provider word |
| 0x5080 | TRAIN_CMD | WO-CMD | — | C | [0] start=1, [3:1] primitive, [11:4] rank/lane, [27:12] options, [31:28] reserved |
| 0x5084 | TRAIN_STATUS | RO | 0 | SN | [2:0] busy/done/error, [15:8] terminal code |
| 0x5088 | TRAIN_TIMEOUT | RW | 0x00000400 | P | nonzero |
| 0x508C | TRAIN_OBSERVATION0 | RO | 0 | SN | |
| 0x5090 | PHY_COMP_NCTL | RO | 0 | LV | [15:0] |
| 0x5094 | PHY_COMP_PCTL | RO | 0 | LV | [15:0] |
| 0x5098 | PHY_COMP_RXCTL | RO | 0 | LV | [6:0] |
| 0x509C | PHY_BRIDGE_ERROR | W1C | 0 | LV | [7:0] sticky code |

### Region 6 — interrupts (5 words)

| Offset | Name | Access | Reset | Notes |
|---|---|---|---|---|
| 0x6000 | IRQ_RAW | RO | 0 | sticky sources [11:0] |
| 0x6004 | IRQ_MASK | RW | 0 | [11:0] |
| 0x6008 | IRQ_STATUS | RO | 0 | RAW & MASK |
| 0x600C | IRQ_CLEAR | W1C | no storage | [11:0]; reads 0 (VP reading of "no storage") |
| 0x6010 | IRQ_TEST_SET | WO-CMD | no storage | sets RAW when IRQ test enabled, else SLVERR |

IRQ sources: 0 CE threshold, 1 ECC UE, 2 physical read timeout, 3 read-return
quarantine, 4 mapping/configuration, 5 refresh emergency, 6 init failure, 7 training
failure, 8 power transition failure, 9 PHY alert/failure, 10 CSR command/interlock,
11 fatal internal.

Word count: 83 + 50 + 13 + 25 + 13 + 21 + 5 = **210** (matches HAS §8.1).

## 4. Field encodings not defined by HAS/MAS

The C header `include/fx1_ddr3/fx1_ddr3_regs.h` gives every offset and documented field.
Encodings that neither HAS nor MAS defines are PROVISIONAL (DEC-13) and live only in
`include/fx1_ddr3/provisional_encodings.h`: INIT_STATUS[10:7] stage, INIT_ERROR bits,
TRAIN_CMD primitive/rank/lane, TRAIN_STATUS codes, TRAIN_OBSERVATION0 packing, POWER_ERROR,
PSTATE / POWER_STATUS[1:0], PHY_BRIDGE_ERROR, ECC_LAST_INFO, QUARANTINE_FIRST_REQ token
kind, FATAL_STATUS[2:0], blocker codes 0xE0–0xE6, FE_PORT_CMD op 2, REFRESH urgency and
REFRESH_STATUS bits, PHY_STATUS packing, QoS class names, SERVICE_COUNT class membership.
The quarantine reasons 0–4, the QUARANTINE_FIRST_REQ_LO/HI layout and FATAL_STATUS[7:3] are
MAS-defined (MAS Tables 6-135/6-137, §6.2.10) and are in the register header.

Read values at reset that come from live state rather than storage: QUIESCE_STATUS reads
0x1C (global idle, mission idle, DFI tokens empty) and ECC_CAP_STATUS reads 0x00010801 with
ECC (0 without); its [1] and [8:2] come from the last snapshot capture (VPL-39).

## 5. Capability constants per profile (HAS Table 8-6)

| Field | NATIVE_R2 | NATIVE_R1 | LEGACY |
|---|---|---|---|
| CAP_FEATURE[0] ECC | build ECC | build ECC | build ECC |
| CAP_FEATURE[2:1] ratio | 2 | 2 | 1 |
| CAP_FEATURE[4:3] profile ranks | 2 | 1 | 1 |
| CAP_FEATURE[6:5] PHY ranks | 2 | 1 | 1 |
| CAP_FEATURE[8:7] map profiles | MAP_PROFILE_MASK (default 3) | same | same |
| CAP_FEATURE[9] live DFI | 0 | 0 | 0 |
| CAP_FEATURE[10] ratio-2 PHY | 1 | 1 | 0 |
| CAP_FEATURE[11] IRQ test | IRQ_TEST_ENABLE | same | same |
| CAP_DEPTH0 | 0x04101008 | 0x04101008 | 0x04101008 |
| CAP_WIDTH0 | 0x00208410 | 0x00208410 | 0x00204410 |

CAP_WIDTH0 decode check: 0x00208410 = row bits 16 ([4:0]), time width 32 ([10:5]),
DFI bytes 16 ([18:11]), AXI ports 4 ([23:19]). Note CAP_WIDTH0[23:19] is the fixed
bundle count 4, not DDR_SYS_N_PORT (open item, see decision log).
CAP_FEATURE default (NATIVE_R2, ECC on, mask 3, IRQ test on):
`1 | 2<<1 | 2<<3 | 2<<5 | 3<<7 | 0<<9 | 1<<10 | 1<<11` = **0x00000DD5**.
