# DDR3 Controller — Firmware Programming Guide (VP)

Audience: firmware and driver developers who bring up and operate the FTEL DDR3 controller on the FX1 virtual platform. The guide describes the behaviour of the VP model (HAS V1.0, MAS V0.1).

Register offsets and fields are in `include/fx1_ddr3/fx1_ddr3_regs.h` (C99). Encodings that the specification leaves undefined are in `provisional_encodings.h`. These are **PROVISIONAL**: always use the macros, never the numbers ([limitations.md](limitations.md) L-14).

## 1. Register access rules

- **Access size.** Use 32-bit, 4-byte-aligned accesses only. A misaligned or narrow access returns SLVERR.
- **Responses.**
  - An unmapped offset returns DECERR, with read data 0.
  - A write to an RO register returns SLVERR and changes nothing.
  - A nonzero write to a "write zero" reserved bit returns SLVERR.
- **Strobes.**
  - RW, SHADOW and INIT_RW registers merge byte strobes; WSTRB = 0 is a legal no-op.
  - W1C and command (WO-CMD) registers need all four strobes; otherwise SLVERR with no effect. Command registers read 0.
- **Command completion.** A command write returns only after its effect is decided:
  - commits: about 4 controller cycles, plus any drain they wait for;
  - FE_PORT_CMD: until the port drains or the hold ends;
  - TRAIN_CMD: until the training episode ends.
  
  A second command issued while one is in progress returns SLVERR (mutex busy, COMMAND_ERROR[0]). Commands are not queued.
- **Snapshot registers** (`[SNAP]` in the header) read 0 until the first capture. Before reading them, write `SNAPSHOT_CMD = 1`. All snapshot registers are captured together at one instant. `SNAPSHOT_STATUS[1]` is valid and `[31:16]` is the capture sequence number.
- **SHADOW and ACTIVE images.** Writing a SHADOW register never changes behaviour; the matching `*_COMMIT` installs it. `*_ACTIVE` registers show the installed image and read 0 while no image is installed.
- **INIT_RW registers** (INIT_* waits, MR0..MR3, PHY delay words, T_CKSRE/T_CKSRX) are writable while initialization is idle (HOLD, DONE or FAILED). They are locked from the start of initialization until it ends, and during runtime training. A locked write returns SLVERR and sets COMMAND_ERROR[2].

## 2. What the platform provides before firmware runs

The DDR3 controller has no CSR for these items (HAS sideband, SPEC-04). In FX1 they come from the platform and from DDR_SYSCTRL at 0x1201_0000 (`fx1/fx1_ddr_sysctrl.h`; integration guide §9.2):

| Item | Why it matters |
|---|---|
| Bootstrap region map (port 0 region 0 = 0x8000_0000 / 512 MiB in FX1) | Memory decode until firmware commits its own map (DEC-07) |
| Per-port QoS table marked configured | A port accepts **no** request (not even an erroneous one) until its table is configured (FE05). A port soft reset clears the table again |
| Reset and lifecycle pins, PHY, P-channel | Coordinated resets, readiness pins, low-power requests |

## 3. Identify the controller

```c
assert(rd(FX1_DDR3_IP_ID) == 0x44445233u);
uint32_t abi  = rd(FX1_DDR3_ABI_VERSION);     /* [31:16] major = 1 */
uint32_t feat = rd(FX1_DDR3_CAP_FEATURE);
bool ecc      = feat & FX1_DDR3_CAP_FEATURE_ECC;
unsigned ratio  = FX1_DDR3_CAP_FEATURE_RATIO(feat);   /* 2 native, 1 legacy */
unsigned ranks  = FX1_DDR3_CAP_FEATURE_RANKS(feat);   /* maximum ranks of the build */
unsigned maps   = FX1_DDR3_CAP_FEATURE_MAP_MASK(feat);/* bit0 linear, bit1 interleaved */
```

`CAP_WIDTH0` holds the maximum row bits [4:0], the time-stamp width [10:5], the DFI data bytes [18:11] and the port bundle count [23:19]. That count is always 4, even when fewer ports are built.

## 4. Firmware-driven initialization

Initialization is not started by a command: `INIT_CMD` always returns SLVERR (DEC-05). It starts by itself, from HOLD, as soon as a valid topology, timing **and** DFI image are all installed. Every later successful commit of one of them restarts the full sequence:

PHY reset → PHY ready → reset-release wait → CKE → MR2/MR3/MR1/MR0 → DLL → ZQCL → boot training → (ECC) startup scrub.

### 4.1 Order of operations

1. Write the timing shadow (19 words) and the DFI shadow (4 words).
2. Write every INIT_RW word: waits, MR0..MR3, PHY delays. This must be done before step 4, because the words lock once initialization starts.
3. Write TOPO_SHADOW and commit the topology.
4. Commit the timing, then the DFI image. The DFI commit checks its words against the **active** timing, so commit the timing first.
5. Set `GLOBAL_CTRL.enable = 1`. Accepted memory transactions wait at the backend until enable is set and the controller is mission-ready.
6. Wait for readiness (§4.6).
7. Optionally commit a region map (§4.7).

Each commit returns SLVERR if it is rejected. Check the commit status word (§4.5).

### 4.2 Timing image (tCK counts)

Use the JEDEC values for the device and speed bin. The VP reference images (4 Gb x8, normal temperature) are:

| Word (`FX1_DDR3_TIMING_SHADOW(i)`) | DDR3-800 | DDR3-1066 | DDR3-1333 | DDR3-1600 |
|---|---|---|---|---|
| T_RCD / T_RP | 6 / 6 | 8 / 8 | 10 / 10 | 11 / 11 |
| T_RAS / T_RC | 15 / 21 | 20 / 28 | 24 / 34 | 28 / 39 |
| T_RRD / T_FAW | 4 / 16 | 4 / 20 | 4 / 20 | 5 / 24 |
| T_CCD / T_RTP | 4 / 4 | 4 / 4 | 4 / 5 | 4 / 6 |
| T_WR / T_WTR | 6 / 4 | 8 / 4 | 10 / 5 | 12 / 6 |
| T_RTW / T_RTRS | 7 / 2 | 8 / 2 | 9 / 2 | 9 / 2 |
| T_RFC / T_REFI | 104 / 3120 | 139 / 4160 | 174 / 5200 | 208 / 6240 |
| CL / CWL / AL | 6 / 5 / 0 | 8 / 6 / 0 | 10 / 7 / 0 | 11 / 8 / 0 |
| PHY_WRITE_LATENCY / PHY_READ_LATENCY | 1 / 6 | 2 / 6 | 2 / 6 | 2 / 6 |

TIMING_COMMIT (MAS Table 5-20) rejects the image with immediate blocker 0x04 in any of these cases:

- a word is ≥ 2^31;
- a word is zero, except T_RTRS, AL and the PHY latencies;
- CL is outside 5..11, CWL outside 5..8, or T_WR outside 5..16;
- AL is not 0, CL−1 or CL−2;
- T_RTW < CL + 6 − CWL;
- T_CCD, T_RRD, T_RTP or T_WTR is < 4.

### 4.3 DFI image

The two delays must equal the formulas computed from the **active** timing; the formulas are also in the header comments:

```
DFI_WR_ENABLE_DELAY = CWL + AL - PHY_WRITE_LATENCY - 2
DFI_RD_ENABLE_DELAY = CL + AL + PHY_READ_LATENCY + G - 4      (G = read gate offset, 0 in FX1)
```

| Word | DDR3-800 | DDR3-1066 | DDR3-1333 | DDR3-1600 |
|---|---|---|---|---|
| DFI_WR_ENABLE_DELAY | 2 | 2 | 3 | 4 |
| DFI_RD_ENABLE_DELAY | 8 | 10 | 12 | 13 |
| PHY_MAX_READ_RETURN | 256 | 256 | 256 | 256 |
| DFI_ASSEMBLY_MARGIN | 1 | 1 | 1 | 1 |

DFI_TIMING_COMMIT also requires:

- both delays ≥ R and < 256;
- the calendar bound;
- PHY_MAX_READ_RETURN ≠ 0;
- a margin of 1..255;
- READ_TIMEOUT ≥ 5R + DFI_RD_ENABLE_DELAY + PHY_MAX_READ_RETURN + 4 + margin. For DDR3-1600 that is 284; the reset value 1024 passes.

Rejections:

- without an active timing image: blocker 0xE5 (provisional);
- with an illegal field: blocker 0xE4.

A later TIMING_COMMIT that no longer matches the DFI image invalidates it. Initialization then waits in HOLD until a matching DFI commit.

### 4.4 INIT_RW words

- **INIT waits count controller cycles** (HAS, SOQ-24), not tCK. The minimum is `ceil(time / (R × tCK))`. For DDR3-1600 at R = 2 (2.5 ns per cycle):

| Word | Requirement | DDR3-1600, R = 2 |
|---|---|---|
| INIT_RESET_RELEASE_WAIT | 500 µs | 200 000 |
| INIT_CKE_LOW_WAIT | max(10 ns, 5 tCK) | 4 |
| INIT_CKE_HIGH_WAIT | tXPR = max(5 tCK, tRFC + 10 ns) | 108 |
| INIT_DLL_WAIT | tDLLK = 512 tCK | 256 |
| INIT_ZQ_WAIT | tZQinit = 512 tCK, per rank | 256 |

  The MAS reference tables give tCK counts; writing those values is safe but waits R times longer. INIT_RESET_ASSERT_WAIT and INIT_CMD_TIMEOUT are read back only; the PHY-reset wait is fixed at 4 cycles.
- **MR0..MR3** carry the JEDEC mode-register payloads. A write returns SLVERR, and keeps the old value, in these cases:
  - MR1 Rtt_Nom {A9,A6,A2} > 3;
  - MR1 output drive {A5,A1} > 1;
  - MR2 Rtt_WR = 11;
  - MR2 with both SRT and ASR set.
- **PHY delays.**
  - PHY_RDLVL_DELAY [23:0], PHY_GATE_DELAY [23:0] and PHY_WRLVL_DELAY [2:0] are loaded during boot training (tag 0, every active rank, all byte lanes, DEC-06).
  - The write-level value is sent as a thermometer code.
  - PHY_SKEW_CTRL0/1 are passed through unchanged.

### 4.5 Topology and commit status

```c
wr(FX1_DDR3_TOPO_SHADOW, FX1_DDR3_TOPO_VAL(2 /*ranks*/, 16 /*row bits*/,
                                           FX1_DDR3_TOPO_PAGE_1024, FX1_DDR3_TOPO_MAP_LINEAR));
if (wr(FX1_DDR3_TOPO_COMMIT, 1) != OK) {
    uint32_t st = rd(FX1_DDR3_TOPO_COMMIT_STATUS);
    /* FX1_DDR3_COMMIT_ST_ERROR; blocker in IMM_BLOCKER ([23:16]) or ACK_BLOCKER ([15:8]) */
}
```

- **Legality.**
  - ranks: 1 .. CAP ranks;
  - row bits: 12..16;
  - page code: 256/512/1024/2048 words;
  - map profile: allowed by CAP_FEATURE[8:7];
  - physical capacity ≤ 2^28 granules;
  - with ECC, at least one usable granule.
  
  Otherwise the commit is rejected with blocker 0x11. The page-code and map-profile encodings await confirmation (SOQ-31).
- **Capacity words** (snapshot): `PHYSICAL_GRANULES = ranks × 2^rows × page_words`; with ECC, `USABLE_GRANULES = min(8 × floor(P/9), 2^27)`; `ECC_BASE` = USABLE (0 without ECC). One granule is 32 bytes. The memory reachable through AXI is `USABLE_GRANULES × 32` bytes per region offset. Anything beyond that returns DECERR.
- **Commit status words.**
  - `*_COMMIT_STATUS[0]` done.
  - `[1]` error.
  - `[2]` reinit-required: set after a successful commit, cleared when initialization finishes.
  - `[15:8]` blocker from the acknowledged path, for example 0xE2 when a reset cancelled the commit.
  - `[23:16]` blocker from the immediate checks.
  
  After any SLVERR, read both blocker fields.

| Blocker | Meaning |
|---|---|
| 0x04 | Timing image illegal (MAS) |
| 0x11 | Topology illegal (MAS) |
| 0x0B / 0x0C / 0x0D | Region: crossing not ready / alignment, overflow or same-port overlap / size beyond USABLE_GRANULES (MAS) |
| 0xE0 | Not quiesced, or work still owned (provisional) |
| 0xE1 | FE_PORT soft reset without quiesce acknowledge (provisional) |
| 0xE2 | Cancelled by a reset (provisional) |
| 0xE3 | Recovery required after a failed power transition (provisional) |
| 0xE4 | DFI field illegal (provisional) |
| 0xE5 | DFI commit without an active timing image (provisional) |
| 0xE6 | FE_PORT_CMD bad port or op (provisional) |

**Quiesce rule.** TOPO, TIMING and DFI commits are accepted only when one of these holds:

- mission admission has not opened since the last (re)initialization and the backend is idle (the boot case);
- a global quiesce is complete (§6.1).

### 4.6 Waiting for readiness

Initialization, boot training and, with ECC, the startup scrub run without firmware involvement. Mission readiness has no CSR bit (L-11), so firmware has two ways to detect it:

- **DDR_SYSCTRL** mirrors the `mission_ready`, `init_complete`, `global_idle` and `fe_drained` pins in its STATUS register:

```c
while (!(rd32(FX1_DDR_SYSCTRL_BASE + FX1_DDRSC_STATUS) & FX1_DDRSC_ST_MISSION_READY)) {
    /* check INIT_STATUS[2] and FATAL_STATUS for failures; sleep; apply a timeout */
}
```
- **Status polling.** Capture a snapshot and read INIT_STATUS:

```c
for (;;) {
    wr(FX1_DDR3_SNAPSHOT_CMD, FX1_DDR3_SNAPSHOT_CMD_CAPTURE);
    uint32_t s = rd(FX1_DDR3_INIT_STATUS);
    if (s & FX1_DDR3_INIT_ST_FAILED) goto init_failed;               /* §7.4 */
    if (!(s & FX1_DDR3_INIT_ST_ADM_BLOCK)) break;                    /* ready, no stop, no fatal */
}
```

`INIT_STATUS[15]` (admission blocked) = not mission-ready OR GLOBAL_CTRL admission stop OR fatal. `INIT_STATUS[10:7]` holds the stage code (`FX1_DDR3_INIT_STAGE_*`, provisional). `[1]` means the DRAM is initialized.

**Startup scrub duration.** With ECC the scrub writes U + U/8 granules (4 tCK each). For 2 ranks of 4 Gb x8 at DDR3-1600 that is 134 M writes, about 0.67 s of simulated time. Set boot watchdogs accordingly.

**Scrub erases memory.** With ECC the scrub zeroes every user and companion granule. Load code or data into DDR only after readiness (SPEC-08). After the scrub, every granule reads back as zero, error-free.

### 4.7 Region map

```c
wr(FX1_DDR3_FE_REGION_SHADOW(FX1_DDR3_REGION_INDEX(0, 0, 0)), 0x80000000u); /* port 0, region 0, BASE */
wr(FX1_DDR3_FE_REGION_SHADOW(FX1_DDR3_REGION_INDEX(0, 0, 1)), 0x20000000u); /* SIZE */
if (wr(FX1_DDR3_FE_REGION_COMMIT, 1) != OK) { /* FE_REGION_STATUS blocker [15:8] */ }
```

- **Prerequisites.** Commit the topology first: region sizes are checked against USABLE_GRANULES, and USABLE_GRANULES is 0 before then.
- **Alignment.** BASE and SIZE must be multiples of 32, and BASE + SIZE ≤ 2^32.
- **Overlap.** Regions of one port must not overlap; regions of different ports may.
- **Address mapping.** Each region maps its first byte to user offset 0, so regions alias each other (SOQ-20, L-13).
- **Commit sequence.** The commit stops new traffic on every port, waits until all accepted transactions drain (indefinitely), installs the whole image at once, and releases the ports.
- **Readback.** FE_REGION_STATUS: [1] done, [16] image installed.
- **Bootstrap readback.** Until the first commit, the bootstrap map decodes, but FE_REGION_ACTIVE reads 0. A core reset returns to the bootstrap map and clears FE_REGION_ACTIVE.

## 5. Memory behaviour seen by software

| Situation | AXI response |
|---|---|
| Address outside every region of the port | DECERR. Read data 0; the write is dropped; IRQ_RAW[4] |
| Region offset ≥ USABLE_GRANULES × 32 (includes the hidden ECC area) | DECERR (no IRQ) |
| Malformed burst: SIZE > bus width, reserved BURST, WRAP length or alignment, FIXED with LEN > 0, INCR crossing 4 KiB, burst leaving the region | SLVERR; memory unchanged |
| Uncorrectable ECC error on a read | SLVERR with zero data for that beat and every later beat |
| Partial write onto an uncorrectable lane it does not fully overwrite | SLVERR, no write |
| Correctable error | Corrected data with OKAY; counted on every read (no write-back) |
| Read quarantine active | Reads SLVERR, zero data. With ECC, writes are also SLVERR (§7.2) |
| Fully masked write (every strobe 0) | OKAY, nothing written |

- **Ordering.**
  - Transactions with the same AXI ID on one port complete in issue order, reads and writes alike.
  - Read data of a port returns in acceptance order.
  - There is no ordering across ports, but a write's B response means the data is visible to every port.
- **Exclusive access** (ports in EXCL_PORT_MASK, port 0 by default):
  - Naturally aligned accesses of at most 32 bytes. A read that creates a reservation returns EXOKAY. A matching exclusive write updates memory and returns EXOKAY.
  - Any other exclusive write returns OKAY without writing.
  - Reservations are cleared by: an ordinary write to the granule, a port quiesce, a region commit, a low-power entry, and port or core reset.
  - On the FX1 CPU path, LR/SC and AMO use the platform monitor; the bridge does not use AXI exclusives. That monitor follows the DDR storage: a store through a region alias voids a reservation taken through another alias, and a cold or core reset, a reinitialization (any TOPO/TIMING/DFI commit) or a region commit voids every DDR reservation, so the next SC fails.
- **Stalls are not errors.** Requests stall (they are not failed) in these cases:
  - while the port's QoS table is not configured;
  - during a port, global or region-commit quiesce, or a soft-reset hold;
  - before readiness or with GLOBAL_CTRL.enable = 0;
  - during runtime training or a low-power state;
  - after a fatal event (until reset).

## 6. Runtime operations

### 6.1 Global quiesce and reconfiguration

```c
wr(FX1_DDR3_QUIESCE_CTRL, FX1_DDR3_QUIESCE_CTRL_REQUEST);    /* new requests held at the ports */
while (!(rd(FX1_DDR3_QUIESCE_STATUS) & FX1_DDR3_QUIESCE_ST_COMPLETE)) {}
/* TOPO / TIMING / DFI commits are accepted now; each restarts full initialization */
wr(FX1_DDR3_QUIESCE_CTRL, FX1_DDR3_QUIESCE_CTRL_RELEASE);
```

`GLOBAL_CTRL[1]` (admission stop) also holds new requests at the ports. Accepted transactions always complete. A reconfiguration with ECC reruns the scrub, which zeroes memory.

### 6.2 Port quiesce and soft reset (FE_PORT_CMD)

```c
wr(FX1_DDR3_FE_PORT_CMD, FX1_DDR3_FE_PORT_CMD_VAL(FX1_DDR3_FE_OP_QUIESCE, port));    /* returns when drained */
wr(FX1_DDR3_FE_PORT_CMD, FX1_DDR3_FE_PORT_CMD_VAL(FX1_DDR3_FE_OP_SOFT_RESET, port)); /* returns after the 8-cycle hold */
/* the soft reset cleared the port's QoS table: the platform must reprogram it */
wr(FX1_DDR3_FE_PORT_CMD, FX1_DDR3_FE_PORT_CMD_VAL(FX1_DDR3_FE_OP_RELEASE, port));
```

- **Soft reset without a prior quiesce acknowledge:** SLVERR with blocker 0xE1.
- **Status after each command:**
  - FE_PORT_CMD_STATUS: busy, done or error, op, port, blocker;
  - FE_PORT_STATE: per-port quiesce, acknowledge, reset and drained bits. It is published on each successful command.
- **Encodings:** op 2 (release) and the blocker codes are provisional.

### 6.3 Runtime training

```c
wr(FX1_DDR3_PHY_GATE_DELAY, new_gate);   /* INIT_RW, allowed while no training runs */
st = wr(FX1_DDR3_TRAIN_CMD, FX1_DDR3_TRAIN_CMD_VAL(FX1_DDR3_TRAIN_PRIM_LOAD_GATE, rank, 0xF /*lanes*/, 0));
code = FX1_DDR3_TRAIN_ST_CODE(snapshot(FX1_DDR3_TRAIN_STATUS));
```

The command first closes readiness. It then waits for the backend to drain, loads the selected PHY delays into the PHY, and returns when the episode ends.

It is rejected with code `FX1_DDR3_TRAIN_CODE_REJECTED` in any of these cases:

- initialization is not done;
- the controller is not in the active power state;
- a training or PHY failure is outstanding;
- recovery is required;
- the rank is absent;
- no lane is selected.

TRAIN_STATUS codes (provisional) are OK, TIMEOUT (TRAIN_TIMEOUT controller cycles), STALE, PHY_FAIL, REJECTED and CANCELLED. A failed episode leaves the rank unavailable (IRQ_RAW[7]) until recovery (§7.4).

### 6.4 Power states

POWER_CMD always returns SLVERR. Low-power states are requested by the system power controller over the **P-channel**. In FX1, firmware writes `FX1_DDRSC_PWR_REQ_VAL(pstate)` to DDR_SYSCTRL PWR_REQ and polls PWR_STATUS until BUSY clears; ACCEPTED or DENIED then tells the outcome. With PWR_CTRL.AUTO_WAKE (reset value 1) DDR_SYSCTRL requests ACTIVE by itself when the controller raises PACTIVE, so a DDR access (also an instruction fetch) after a low-power entry simply waits for the wake. If the wake is denied, for example because a failed exit left POWER_STATUS[5] (recovery required), PWR_STATUS[3] is set and the request is retried with a back-off; the access stays held until a cold reset (RST_CTRL) aborts it. Code that must recover from this runs on another hart, from BootROM/SRAM (`fw/fx1_soc/tests/ddr3/power_recovery.c`). The PSTATE values are provisional:

- 0 ACTIVE;
- 1 active power-down;
- 2 precharge power-down;
- 3 self-refresh.

- **Entry.** Admission closes and the backend drains. If the drain exceeds POWER_TIMEOUT (controller cycles), the request is denied before the irreversible step: POWER_ERROR pre-cutoff timeout, IRQ_RAW[8]. A request while initialization, scrub or training runs, or while a command holds the mutex, is denied as busy.
- **Exclusive reservations.** Every entry invalidates the exclusive reservations.
- **Self-refresh** stops the DRAM clock and holds the refresh debt.
- **Wake** waits for the exit timing and PHY ready. A timeout after the irreversible step sets POWER_STATUS[5] (recovery required): admission stays closed until a reset.
- **Retention.** Data, images and training context are retained across low-power states; there is no retraining.

### 6.5 Refresh

- **REFRESH_CFG[3:0]** is the debt limit (1..8).
- **REFRESH_RANK0/1** (snapshot): [4:0] signed debt (`FX1_DDR3_REFRESH_RANK_DEBT()`), [6:5] urgency (provisional codes).
- **REFRESH_STATUS:** per-rank mandatory, any high, any mandatory, saturation.
- **Saturation** sets IRQ_RAW[5]. Debt grows only while refresh cannot be issued (power-down, or injected faults); traffic otherwise sees normal tRFC windows.

## 7. Interrupts and error handling

`irq` is a level: OR(IRQ_RAW & IRQ_MASK). IRQ_MASK resets to 0.

- **Clearing.** Clear a source with a full-word W1C to IRQ_CLEAR. A hardware set in the same cycle wins.
- **Sticky causes stay.** Clearing an interrupt never clears the underlying sticky word: COMMAND_ERROR, INIT_ERROR, POWER_ERROR, PHY_BRIDGE_ERROR, *_COMMIT_STATUS, FATAL_STATUS, counters. Clear those words separately (W1C) where they are W1C.
- **IRQ_TEST_SET.** When CAP_FEATURE[11] is set, IRQ_TEST_SET sets IRQ_RAW bits for driver testing.

| Bit | Source | Typical handling |
|---|---|---|
| 0 | CE_COUNT reached CE_THRESHOLD (crossing, reset 1, nonzero) | Snapshot CE_COUNT / ECC_LAST_*; scrub or retire the page in software |
| 1 | Uncorrectable ECC error | Snapshot UE_COUNT / ECC_LAST_ADDR (physical data granule) / ECC_LAST_INFO; the access already returned SLVERR |
| 2 | Read-return timeout → quarantine | §7.2 |
| 3 | Malformed / unexpected / stale read return → quarantine | §7.2 |
| 4 | Commit validation reject or region decode miss | Read the commit status blocker / fix the address |
| 5 | Refresh debt saturation | Wake the controller, investigate blocked refresh |
| 6 | Initialization failure | §7.4 |
| 7 | Training failure | §7.4 |
| 8 | Power transition failure | POWER_ERROR; recovery if POWER_STATUS[5] |
| 9 | PHY failure (rising edge) | §7.4 |
| 10 | CSR command/interlock error | COMMAND_ERROR: [0] busy, [1] blocked, [2] commit/lock |
| 11 | Fatal containment | §7.3 |

### 7.1 ECC

- **Status.** ECC_CAP_STATUS [0] gives ECC presence; [12:9] = 4 codewords and [20:13] = 8 bytes per codeword. [1] RMW busy and [8:2] locks are snapshot fields.
- **Counters.** CE_COUNT counts corrected 64-bit lanes and UE_COUNT counts events; both saturate. Both are cleared by a core reset, not by a CSR reset (SOQ-38).
- **Last error.** ECC_LAST_ADDR_LO/HI hold the physical granule index of the last event. ECC_LAST_INFO (provisional) holds the type [1:0] (CE 1, UE 2), the lowest affected lane [3:2], and {mismatch, syndrome} [15:8].
- **No write-back.** Corrected data is not written back. Firmware that wants to repair a location rewrites it with a full 32-byte write.

### 7.2 Read quarantine

A malformed, stale, unexpected or missing (READ_TIMEOUT tCK) read return quarantines reads:

- **QUARANTINE_STATUS:** [1:0] = 3 while active, [4:2] = reason (0 timeout, 1 malformed, 2 unexpected/tokenless, 3 context, 4 generation).
- **QUARANTINE_FIRST_REQ_LO/HI:** first cause (index, generation, kind, reason, valid) and the head deadline. This record is latched once.
- **During quarantine:**
  - new reads return SLVERR with zero data;
  - writes without ECC continue;
  - writes with ECC return SLVERR, because they need the companion read (SOQ-37).
- **Recovery:** the quarantine survives a core reset. Only a **cold reset** followed by full initialization clears it. In FX1: write `FX1_DDRSC_RST_COLD` to DDR_SYSCTRL RST_CTRL, wait for RST_STATUS.BUSY to clear, configure the QoS table again (QOS_LUT, QOS_CFG), then §4. With ECC the new scrub zeroes DDR, so this code must run from BootROM/SRAM.

### 7.3 Fatal containment

- **Status.** FATAL_STATUS is a sticky bitmap with no software clear:
  - [0] ECC hold, provisional — a write-result failure after an ECC write;
  - [1] scrub fatal, provisional;
  - [3] DFI protocol fault;
  - [6] write-data protocol fault;
  - [7] unmatched write result.
- **Effect.** Mission readiness drops (INIT_STATUS[15]). Transactions that have not launched are **held, not answered**. Issued writes are not rolled back.
- **Recovery.** FATAL_STATUS and the fatal hold clear only on a **cold reset**. A core reset aborts the held transactions with SLVERR, but readiness stays closed.
  1. The platform performs the cold reset (FX1: DDR_SYSCTRL RST_CTRL = `FX1_DDRSC_RST_COLD`). Where a coordinated core reset is used first (`FX1_DDRSC_RST_CORE`), held transactions do not prevent reset-safe and are answered with SLVERR. A CPU whose access is held cannot issue the reset itself: another hart, or a watchdog, must.
  2. Program and commit again (§4).

### 7.4 Initialization, training and PHY failures

- **Initialization failure** (INIT_STATUS[2], IRQ_RAW[6]):
  - INIT_ERROR (provisional, W1C) has one bit per failing stage.
  - Correct the configuration, then commit again. Every commit restarts initialization; the INIT_RW words are writable while FAILED.
- **Training failure** (IRQ_RAW[7]) or PHY failure (IRQ_RAW[9]):
  - Readiness closes and the rank is unavailable.
  - Recover with a quiesce plus recommit, or with a reset. Either path reruns initialization and training.
- **PHY bridge errors:** PHY_BRIDGE_ERROR (provisional bitmap) reports load timeout, stale/wrong identity, provider failure and load while not ready.

## 8. Bring-up checklist

1. Platform: bootstrap map set, QoS table of every used port configured, resets released (FX1: done at power-up; after a cold reset, configure QoS through DDR_SYSCTRL).
2. Check IP_ID, ABI major 1 and CAP_FEATURE (ECC, ranks, map profiles).
3. Write the timing shadow and the DFI shadow.
4. Write the INIT_RW words: waits in controller cycles, MR0..MR3, PHY delays.
5. Write TOPO_SHADOW; commit topology, then timing, then DFI. Check each SLVERR and the blocker fields.
6. Set GLOBAL_CTRL.enable.
7. Wait for readiness (DDR_SYSCTRL, or snapshot + INIT_STATUS[15] = 0). Allow for the scrub time.
8. Optionally commit the region map.
9. Unmask the interrupts you handle (IRQ_MASK). Keep IRQ_RAW[11], [6], [1], [2] and [3] enabled.
10. Load DDR contents only now.

## 9. FX1 reference code

`fw/fx1_soc/ddr3/ddr3_fw.c` implements §4 step by step with the DDR3-1600 reference image of `ddr3_ref_1600.h` (also checked against the model by `test_fx1_ddr3_subsystem refimage`). The CPU tests in `fw/fx1_soc/tests/ddr3/` show each path on the VP:

| Test | Shows |
|---|---|
| `fw_init.c` (BootROM, `--ddr-boot firmware`) | §3, a rejected TIMING_COMMIT (SLVERR, blocker 0x04, IRQ_RAW[4]), §4, readiness through DDR_SYSCTRL, region decode miss → load access fault + PLIC source 8, self-refresh and automatic wake, two harts |
| `fault_recovery.c` (BootROM) | §7.1 CE and UE, §7.2 read timeout → quarantine → cold reset + re-initialization, §7.3 fatal hold → core reset by the other hart → cold reset |
| `preinit.c` (DDR, default boot) | The state the platform leaves, CE/UE with PLIC interrupts, self-refresh while executing from DDR |
| `boot/boot_loader.c` | A BootROM loader: §4 bring-up, then copy of an ELF from BootROM to DDR; failures reported from BootROM/SRAM |
