# DDR3 Controller VP — Interface Contracts

Audience: integrators and test writers. These contracts are fixed by subtask 1; later subtasks
fill in behaviour behind them.

## 1. CSR port (AXI4-Lite, HAS §8)

| Item | Contract |
|---|---|
| Socket | `csr_socket`, `tlm_utils::simple_target_socket`, `b_transport` + `transport_dbg` |
| Address | 16-bit byte offset inside the 64 KiB aperture; the bus removes the SoC base |
| Access size | Exactly 4 bytes, 4-byte aligned. Any other length or alignment → `TLM_GENERIC_ERROR_RESPONSE` (SLVERR) |
| Strobes | `byte_enable` = WSTRB (4 entries, 0xFF = lane enabled); absent = 0xF |
| Responses | OKAY → `TLM_OK_RESPONSE`; SLVERR → `TLM_GENERIC_ERROR_RESPONSE`; DECERR → `TLM_ADDRESS_ERROR_RESPONSE` (read data 0) |
| Access latency | Every access returns one controller cycle after it is decided (VPL-05); the call must come from an SC_THREAD |
| Command latency | Commands that HAS defines as "write response waits for the endpoint result" block the `b_transport` call until that result: commits and snapshots 4 controller cycles (VPL-06), FE_PORT_CMD until drain/hold end, TRAIN_CMD until the training episode ends |
| Malformed payload | Checked before any buffer access: command other than read/write → `TLM_COMMAND_ERROR_RESPONSE`; null data pointer → `TLM_GENERIC_ERROR_RESPONSE`; byte-enable length other than 4 → `TLM_BYTE_ENABLE_ERROR_RESPONSE`. These are initiator/integration errors, not AXI4-Lite responses |
| Buffer direction | Read data is written only for reads (zero on error); a write's data buffer is never modified, accepted or not |
| During reset | Access while the CSR domain (or cold reset) is asserted → `TLM_GENERIC_ERROR_RESPONSE` plus a warning (integration error) |
| Debug | `transport_dbg` reads registers without side effects (no snapshot capture, no W1C); writes are rejected (returns 0 bytes) |
| Field encodings | Offsets and fields: `include/fx1_ddr3/fx1_ddr3_regs.h` (C99). Undefined-by-spec encodings: `provisional_encodings.h` (DEC-13) |

## 2. Memory ports (AXI4, HAS §2.2, §6.1)

| Item | Contract |
|---|---|
| Sockets | `mem_socket[0..3]`, `simple_target_socket_tagged_optional`; only ports `< n_ports` may be bound (elaboration error otherwise, FE01) |
| Transaction | One `b_transport` = one AXI transaction described by `fx1::ddr3::AxiExtension::req` |
| Address | `req.addr` and the payload address are the **global** AXI address (DEC-07) |
| Data layout | Lane image: `data_length = (len+1) × beat_lanes`, beat n at `[n × beat_lanes, (n+1) × beat_lanes)`, each byte at its bus lane; `beat_lanes` = AXI_DATA_WIDTH/8 (16 native, 8 legacy) |
| Write strobes | `byte_enable` of the same length = WSTRB; null = all lanes active for the transfer size and address |
| Read data | Active lanes carry data; inactive lanes return 0 |
| Response | `ext.resp` = B response or the worst R response; `ext.beat_resp` = per R beat (sticky-forward, SOQ-23; an error beat carries zero data); TLM status mirrors `resp` |
| Missing extension | `TLM_COMMAND_ERROR_RESPONSE` (protocol error at the IP boundary) |
| Model-usage errors | Rejected before any buffer access (VPL-34): command, payload address or ID width disagreeing with the extension → `TLM_COMMAND_ERROR_RESPONSE`; null data → `TLM_GENERIC_ERROR_RESPONSE`; `data_length` ≠ (LEN+1) × beat_lanes → `TLM_BURST_ERROR_RESPONSE`; byte-enable length ≠ `data_length` → `TLM_BYTE_ENABLE_ERROR_RESPONSE` |
| Blocking | While admission is closed the call blocks in simulated time (AXI stall). It never turns a stall into an error. Two levels (VPL-18): the port gate (stops, QoS gate, 32 contexts per port) holds new calls; the backend gate (GLOBAL_CTRL.enable, mission readiness) holds accepted legal calls. Decode errors complete without the backend |
| Order | Same-ID chain per port across reads and writes; R data per port in acceptance order; no cross-port order (VPL-19). A B response means the data is visible to every port |
| Latency | Decode errors: none. Backend work: per-bank DRAM timing (VPL-20) plus 4 controller cycles. The call returns with `delay = 0` after consuming the incoming annotation |
| Exclusive | `req.exclusive`; EXOKAY in `ext.resp` with `TLM_OK_RESPONSE` |
| DMI | Not offered (baseline): every access sees admission, ECC, faults and reset |
| Debug | `transport_dbg` uses the effective region map, bypasses admission/timing/exclusive state, generates ECC on writes, raises no IRQ, never opens admission |

### 2.1 Platform bridge (generic bus → port 0)

The FX1 bus carries generic payloads (CPU 1/2/4/8 bytes, DMA bursts) on a 32-bit TLM socket.
`fx1::ddr3::GenericAxiBridge(name, axi_beat_bytes, address_offset = 0, qos = 0)`
(`generic_axi_bridge.h`; target socket `target`, initiator socket `initiator`) converts them
with `plan_generic_access()` after adding `address_offset` (0 when the bus forwards global
addresses):

- A naturally aligned power-of-two access no wider than the AXI data width becomes one narrow
  single-beat INCR (AxSIZE = log2(length)).
- Anything else becomes full-width INCR bursts with strobes limited to the wanted bytes, split at
  every 4 KiB boundary and at 256 beats.
- Fixed attributes: ID 0 (keeps platform traffic in issue order), QoS from the constructor
  (default 0), lock 0.
- Every burst is issued even after one fails (no rollback). Responses: worst AXI response of
  the bursts. SLVERR → `TLM_GENERIC_ERROR_RESPONSE` and DECERR → `TLM_ADDRESS_ERROR_RESPONSE`;
  the CPU port adapter turns both into access faults.
- Generic byte enables are honoured with the TLM repeat rule; streaming width below the data
  length is a burst error. No DMI; `transport_dbg` is forwarded with the translated address.
- The bridge is outside the IP. Platform LR/SC/AMO keep using the platform exclusive monitor,
  so the bridge never sets `exclusive`.

## 3. Interrupt and lifecycle levels

| Port | Type | Meaning |
|---|---|---|
| `irq` | `sc_out<bool>`, must be bound | Active-high level = OR(IRQ_RAW & IRQ_MASK) |
| `mission_ready_out` | optional `sc_signal_inout_if<bool>` port | Mission admission open |
| `global_idle_out` | optional | No accepted work in the controller |
| `fe_drained_out` | optional | Frontend drained |
| `init_complete_out` | optional | BE08 init_complete (also passed to the PHY provider) |

Getters `mission_ready()`, `global_idle()`, `fe_drained()`, `init_complete()`, `irq_level()`
return the same values for benches without signals. One SC_METHOD writes the ports, so a
port shows a change one delta after the getter (VPL-08).

## 4. Sideband controls (HAS Table 1-3/1-4; driven by DDR_SYSCTRL or a bench)

| API | HAS bundle | Effect |
|---|---|---|
| `set_bootstrap_region(port, region, {base, size})` | Bootstrap map | Decode map until the first region commit and after core offline (DEC-07) |
| `qos_write(port, index, cls)`, `qos_set_configured(port, bool)` | QoS table programming | Port admission is blocked until configured (FE05) |
| `set_reserved_rate(port, rate, depth)` | Reserved-rate programming | 20-bit rate/depth; stored (service floor abstracted, GAP-FE-23) |
| `pchannel_request(preq, pstate)`, `pchannel()` | P-channel (RD-PWR-01) | Power lifecycle requests and PACCEPT/PDENY/PACTIVE |

QoS classes are plain 2-bit codes (`QosClass` = `uint8_t`, 0..3). Only NORMAL = 1
(`kQosClassNormal`) is derivable from the documents; codes 0, 2 and 3 are stored without an
assigned meaning until the spec owner answers SOQ-13.

## 5. Reset and quiesce bundle (HAS Table 1-3, §7.1 Table 7-4, §6.1.14; IF-1.5-04)

Functional API on `Ddr3Controller`, driven by the platform reset controller and DDR_SYSCTRL
(GAP-LC-15) or by a bench. Reset inputs are levels; `true` = asserted (active-low pins
inverted). `lifecycle_event()` is notified (delta) whenever an acknowledgement level changes.

| API | HAS signal | Contract |
|---|---|---|
| `set_cold_reset(bool)` | cold mailbox reset `cdc_por_n` (+ all domains) | Resets every domain and the endpoint alignment state; QoS tables to reset policy. The model powers up as if all resets were pulsed before time 0 |
| `set_core_reset(bool)` | mission/core `rst_n` | Resets core lifecycle state (init returns to HOLD, readiness and init_complete low, pending core and port completions dropped by epoch). Memory requests stall until release |
| `set_csr_reset(bool)` | `csr_aresetn` | Resets the CSR domain; CSR accesses during the reset are integration errors |
| `set_port_reset(p, bool)` | AXI port `aresetn[p]` | Resets that port domain; accesses on it during the reset are errors |
| `core_reset_request(bool)` / `core_reset_safe()` | core reset request / core-safe | Request stops admission on all ports; safe = request ∧ core not in reset ∧ core and every enabled port drained (HAS §7.1, LC-7.1-03). Dropping the request clears safe. No timeout (GAP-LC-14) |
| `csr_reset_request(bool)` / `csr_reset_safe()` | CSR reset request / CSR-safe | Safe = request ∧ CSR domain not in reset ∧ no CSR command or response pending |
| `port_quiesce_request(p, bool)` / `port_quiesce_ack(p)` | `port_quiesce_req[p]` / `port_quiesce_ack[p]` | Level request; stops new AW/AR on the port; ack when the port owns no work (FE-6.1.14-02); released by dropping the request. Effective quiesce = this request OR the CSR FE_PORT_CMD quiesce (HAS Table 8-8) |
| `port_soft_reset_request(p, bool)`, `port_soft_reset_pending(p)`, `port_soft_reset_active(p)` | `port_soft_reset[p]` | A rising request becomes pending; it starts only while the quiesce ack is high (FE-6.1.14-03) and holds 8 controller cycles even if the request drops (FE-6.1.14-04). A request dropped before it starts is discarded. Completion resets the port context and its QoS table (MAS T6-64). Backend-owned work is never deleted (LC-7.1-04) |

Pending-transaction rules:

- A reset never erases DRAM contents (plan §4.4). Retention across resets is not promised to
  firmware (LC-7.1-07): initialization, training and scrub rerun before admission reopens.
- An access on a port whose own AXI domain is in reset (`set_port_reset`, or cold reset) is an
  integration error (SLVERR plus a warning); so is a CSR access during CSR or cold reset.
- Admission stop conditions: physical core reset, core reset request (all ports), port quiesce
  request (sideband or FE_PORT_CMD), port soft-reset hold, QoS table not configured (FE05). While any applies, the port does not accept requests: the call
  blocks in simulated time and proceeds only after the last applicable condition is released
  (HAS §7.1, §6.1.14, §8.3; FE-6.1.14-01/05, LC-7.1-02). Releasing the physical reset or ending
  the hold does not release a request that is still asserted. A blocked call is aborted with
  SLVERR if the port's own domain enters reset meanwhile. A blocked (not yet accepted) call is
  not owned work, so it does not delay the quiesce ack or reset-safe.
- After a fatal event, accepted transactions that have not launched (waiting at the backend
  gate, for a request slot or a granule lock, or an ECC write before its data write) are held,
  never completed; launched work finishes. Transactions waiting behind held work (same-ID
  chain, read-return order) are held too. Only a core, port or cold reset releases held work,
  with SLVERR. Held work does not block core reset-safe (VPL-41).
- Writes with every strobe zero complete locally in their same-ID turn without the backend
  (OKAY, or EXOKAY for a matching exclusive write, which keeps the reservation; VPL-42). An
  exclusive-write miss completes locally with OKAY. Exclusive verdicts, ordinary-write
  invalidation and exclusive-read reservations follow backend install order (VPL-30), so
  concurrent exclusive and ordinary writes always resolve to a legal serialization.
- Transactions already accepted when a core reset or a reset of their port asserts finish
  with SLVERR and a warning (GAP-FE-21, VPL-33). Data effects already applied (for example the
  granules of a write that had reached the backend) remain; nothing is rolled back. Tests
  must not rely on how much of an aborted write landed.
- Completions scheduled before a reset are dropped by the epoch mechanism; none can complete
  a transaction or lifecycle episode started after the reset.
- CSR commands in progress (commits, quiesce, FE_PORT_CMD, snapshot, TRAIN_CMD) end with
  SLVERR when a reset of their CSR, core or port domain interrupts them (VPL-16). After a core
  or port reset the command status reports blocker 0xE2 (cancelled) and the command mutex is
  free; after a CSR reset all CSR-side words are at reset. A P-channel request pending at a
  core reset is answered with PDENY.

Drain terms (subtask 3): a port owns work while it holds an accepted transaction (frontend
context); the core owns work while a transaction is in backend service. Port quiesce ack,
FE_PORT_STATE drained, `fe_drained`, `global_idle`, reset-safe, QUIESCE_STATUS, the region
publication, power entry and runtime training all use these terms. A port quiesce also
invalidates the port's exclusive reservations.

## 6. PHY provider (HAS Table 1-4, BE07–BE09, BE18)

`fx1::ddr3::PhyProvider` is implemented by the PHY model; the controller implements `PhyClient`.

| Direction | Call | Contract |
|---|---|---|
| ctrl → PHY | `set_phy_reset_n`, `set_clock_disable`, `set_init_complete` | Native lifecycle levels. `clock_disable` is set only after a completed self-refresh entry and cleared at the wake and on reinitialization (VPL-44) |
| ctrl → PHY | `load(PhyLoadRequest)` | One outstanding at a time; payload and SKEW words held until the observation; rank, epoch (16 bit), tag (4 bit, 0 = boot), byte-lane mask (boot: all lanes; runtime: TRAIN_CMD[8:5]) and opaque options (TRAIN_CMD[27:12]) |
| PHY → ctrl | `phy_observation(PhyObservation)` | Echoes rank/epoch/tag; the controller rejects mismatches as stale (training failure) |
| PHY → ctrl | `phy_levels_changed(PhyLevels)` | reset_done, ready, failure, timing_wait_ok |
| ctrl reads | `provider_cap_word`, `provider_status_word`, `comp_*` | Opaque provider words; the controller neither assigns nor interprets bits. How they appear at PHY_CAP/PHY_STATUS is pending SOQ-12 |

`FunctionalPhy` is the default provider. Its host-side fault script covers Fail, Lose,
StaleEpoch, WrongRank, WrongTag and Delay per load, plus forced failure and readiness loss.

## 7. Host debug and fault API

Separate from the CSR ABI, used by benches and the platform CLI. Firmware never depends on it.

| API | Contract |
|---|---|
| `debug_write(user_offset, data, len)` / `debug_read(...)` | User (region-normalized) byte offsets in [0, USABLE_GRANULES × 32) of the committed topology; false without one or outside the range. Writes regenerate the companion record of every touched granule (consistent ECC); reads return the stored bytes uncorrected. No interrupt, admission, order or reservation effect. Preload after the startup scrub: data written before the scrub frontier passes it is erased (warning), as in hardware (VPL-28) |
| `mem_socket[p]` `transport_dbg` | Global address through the port's effective region map; the whole range must lie in one region, else 0 bytes |
| `resident_pages()` | Host pages of the sparse store (4 KiB each) |
| `fault_flip_data_bit(granule, bit)` / `fault_flip_check_bit(granule, lane, bit)` | Corrupt stored data bit 0..255 or check bit 0..7 of one lane's record, without updating ECC. False when not addressable (or without ECC for check bits) |
| `fault_read_return(f, count)` | First granule read of each of the next `count` AXI read transactions entering the backend (not the internal reads of ECC writes): `Timeout` (return lost; quarantine reason 0 at issue + READ_TIMEOUT tCK), `Malformed` (1), `ContextMismatch` (3), `GenerationMismatch` (4) at the return time; `Unexpected` (2) is a tokenless return injected at once |
| `fault_write_result(count)` | Next `count` granule writes report a write-result error: SLVERR; with ECC also the ECC fatal hold and a held lock |
| `fault_fatal(f)` | `DfiProtocol` (FATAL_STATUS[3]), `WriteDataProtocol` ([6]), `WriteResultUnmatched` ([7]), `StaleLockRelease` ([0]) at once; `ScrubMapping` ([1]) fails the running scrub or the next one |
| `fault_clear()` | Disarms pending read, write and scrub faults |
| `fault_block_refresh(rank, bool)` | Refresh deadlines missed while blocked (BE04) |
| Observations | `ce_count()`, `ue_count()`, `quarantined()`, `fatal()`, `fatal_status()`, `scrub_progress()`, `scrub_total()`, `scrub_running()`, `bank_timer()` (row hit/miss/conflict counters) |

`FunctionalPhy` keeps its own fault script (§6).

## 8. Platform integration contract (`fx1_soc`, subtask 5)

| Item | Value |
|---|---|
| DDR data | `MEMCTL_DDR` 0x80000000 / 512 MiB on SysBus1, `GenericAxiBridge` (address offset 0x80000000: the bus removes the base, the port sees global addresses, VPL-53) → port 0 |
| DDR CSR | `DDR3_CSR` 0x12000000 / 64 KiB on SysBus0, translated to the 16-bit offset |
| DDR_SYSCTRL | 0x12010000 / 64 KiB on SysBus0 (DEC-04) |
| IRQ | PLIC source 8 (`FX1_PLIC_NUM_SOURCES` 8 → 9) |
| Boot SRAM | `SRAM` 0x01000000 / 1 MiB on SysBus1 |
| Boot modes | `--ddr-boot preinit` (default) and `--ddr-boot firmware` (DEC-09; integration guide §9.1) |
| Bootstrap map | port 0 region 0 = 0x80000000 / 0x20000000 |
