# DDR3 Controller VP — Fault Catalogue

Audience: firmware/driver developers and test writers who exercise the error paths of the controller on the VP.

Every fault in this file is injected from the host (bench or platform CLI) through the controller or `FunctionalPhy` API. Firmware cannot inject any of them. Faults are deterministic: the same injection at the same simulated time gives the same result. Codes marked *(prov.)* are provisional encodings (DEC-13).

Notation:

- `d` = `Ddr3Controller&`
- `phy` = `*d.functional_phy()`
- user granule = region-normalized byte offset / 32

## 1. Memory data faults (ECC)

| Fault | Injection | Firmware observes | Recovery | Test |
|---|---|---|---|---|
| Single-bit data error | `d.fault_flip_data_bit(granule, bit)` (bit 0..255) | Reads return corrected data with OKAY. CE_COUNT counts one per read; there is no write-back. ECC_LAST_ADDR = the granule; ECC_LAST_INFO = CE, lane, {mismatch, syndrome} *(prov.)*. IRQ_RAW[0] when CE_COUNT reaches CE_THRESHOLD | A full 32-byte rewrite of the granule, or a partial write (re-encodes merged data) | `ecc ecc`, `ecc_legacy` |
| Check-bit / parity error | `d.fault_flip_check_bit(granule, lane, bit)` (bit 7 = overall parity) | As a single-bit data error. Data is unchanged; the syndrome is the position of the check bit (0 for the parity bit) | As above | `ecc ecc` |
| Double-bit error in one lane | Two `fault_flip_data_bit` calls in one 8-byte lane | Read: SLVERR with zero data, from the failing beat to the end of the burst. UE_COUNT +1, IRQ_RAW[1]. A partial write that keeps bytes of that lane: SLVERR, nothing written. A write that fully overwrites the lane heals it (counted as corrected) | A full rewrite | `ecc ecc` (incl. MAS T6-155) |
| Without ECC | `fault_flip_data_bit` | Data is returned as stored. `fault_flip_check_bit` returns false | — | `ecc ecc_off` |

## 2. Read-return faults (quarantine)

These apply to the first granule read of the next `count` AXI read transactions: `d.fault_read_return(f, count)`.

| `f` | Firmware observes | Test |
|---|---|---|
| `Timeout` | The read returns SLVERR with zero data at issue + READ_TIMEOUT tCK. QUARANTINE_STATUS = active, reason 0. IRQ_RAW[2] | `ecc quarantine`, `quarantine_walk` |
| `Malformed` | SLVERR when the return arrives. Reason 1, IRQ_RAW[3] | `ecc quarantine_reasons` |
| `ContextMismatch` | Reason 3, IRQ_RAW[3] | same |
| `GenerationMismatch` | Reason 4, IRQ_RAW[3] | same |
| `Unexpected` (immediate, `count` ignored) | Quarantine at once. Reason 2; first-cause record holds only {reason, valid} | same |

All five faults share the following behaviour:

- **First cause.** QUARANTINE_FIRST_REQ_LO/HI latch the first cause once: request index, generation, kind *(prov.)*, reason, valid, and the deadline.
- **Reads in flight.** Reads whose return comes after the quarantine instant end with SLVERR.
- **New reads** return SLVERR at once.
- **ECC writes** (and ECC writes still waiting for their internal reads) return SLVERR with no write.
- **Writes without ECC** continue.
- **Local completions.** Fully masked writes and exclusive misses still complete with OKAY.
- **Recovery:** cold reset plus full initialization. A core reset does not clear the quarantine.

## 3. Write-result and fatal faults

| Fault | Injection | Firmware observes | Recovery | Test |
|---|---|---|---|---|
| DFI write-result error, no ECC | `d.fault_write_result(count)` | The write returns SLVERR; data is written (no rollback); no fatal | None needed | `ecc fatal_noecc` |
| DFI write-result error, ECC | `d.fault_write_result(count)` | SLVERR. FATAL_STATUS[0] *(prov.)*, IRQ_RAW[11]. The granule lock stays held (ECC_CAP_STATUS[8:2] after a snapshot). Readiness closes. Queued transactions are held, not answered (§5) | Cold reset + initialization | `ecc fatal` |
| DFI protocol fault | `d.fault_fatal(FatalFault::DfiProtocol)` | FATAL_STATUS[3], IRQ_RAW[11], fatal hold | Cold reset | `ecc fatal_sources`, `fatal`, `fatal_containment(_noecc)`, `fatal_followers` |
| Write-data protocol fault | `FatalFault::WriteDataProtocol` | FATAL_STATUS[6], IRQ_RAW[11], fatal hold | Cold reset | `ecc fatal_sources` |
| Unmatched write result | `FatalFault::WriteResultUnmatched` | FATAL_STATUS[7], IRQ_RAW[11], fatal hold | Cold reset | `ecc fatal_sources`, `fatal`, `verify admission_block` |
| Stale lock release | `FatalFault::StaleLockRelease` | FATAL_STATUS[0] *(prov.)*, IRQ_RAW[11], fatal hold | Cold reset | `ecc fatal_sources` |
| Scrub mapping fault | `FatalFault::ScrubMapping` (fails the running scrub, or arms the next one) | FATAL_STATUS[1] *(prov.)*. The scrub stops; readiness never opens | Cold reset | `ecc fatal` |

FATAL_STATUS is a sticky bitmap. Software cannot clear it.

## 4. Lifecycle and PHY faults

| Fault | Injection | Firmware observes | Recovery | Test |
|---|---|---|---|---|
| PHY training failure | `phy.script_load_faults({PhyLoadFault::Fail})` | Boot: INIT_STATUS failed, stage FAILED *(prov.)*, INIT_ERROR stage bit, IRQ_RAW[6] + [7]. Runtime: TRAIN_CMD SLVERR, TRAIN_STATUS PHY_FAIL / code *(prov.)*, IRQ_RAW[7] | Recommit (boot) or quiesce + recommit / reset (runtime) | `lifecycle init_fail`, `runtime_training` |
| Lost training response | `PhyLoadFault::Lose` | TRAIN_TIMEOUT expiry: training failure, PHY_BRIDGE_ERROR[0] *(prov.)*, IRQ_RAW[7] (boot also IRQ_RAW[6]); a runtime TRAIN_CMD ends with TRAIN_STATUS TIMEOUT *(prov.)* | As above | `lifecycle init_fail`, `cancel_training`; provider side `tlm phy_faults` |
| Stale / wrong-identity response | `StaleEpoch`, `WrongRank`, `WrongTag` | Stale epoch: PHY_BRIDGE_ERROR stale, discarded, then timeout. Wrong rank or tag: PHY_BRIDGE_ERROR stale and training failure | As above | `lifecycle init_fail`; provider side `tlm phy_faults` |
| Late response | `PhyLoadFault::Delay` | Success after `delayed_latency` when TRAIN_TIMEOUT is long enough; otherwise a timeout failure | — | `lifecycle init_fail`; provider side `tlm phy_faults` |
| PHY failure level | `phy.force_failure(true)` | IRQ_RAW[9] on the rising edge. Readiness closes. An active training episode ends with PHY_FAIL; during init, init fails | Reset / reinitialization | `lifecycle phy_failure`, `phy_failure_training` |
| PHY readiness loss | `phy.force_ready(false)` | Readiness closes. A wake that waits for PHY ready times out after the cutoff: POWER_STATUS[5] recovery required | Restore ready; reset if recovery is required | `lifecycle power` |
| Refresh blocked | `d.fault_block_refresh(rank, true)` | Debt grows (REFRESH_RANK, snapshot). Urgency rises; PACTIVE rises at high urgency in power-down. Saturation: REFRESH_STATUS[4], IRQ_RAW[5]. No REF windows while blocked | Unblock: the debt is repaid with back-to-back REF | `lifecycle refresh`; repayment busy time `datapath_core bank_timer` |

## 5. Containment behaviour shared by the fatal faults

After any fatal event:

- **Not yet launched.** Transactions that have not launched are held without a response. This covers those waiting at the backend gate, for a request slot or a granule lock, and ECC writes before their data write. It also covers transactions waiting behind them in the same-ID chain or in read-return order.
- **Already launched.** Reads in flight and issued writes finish normally.
- **Reset-safe.** A core reset request becomes reset-safe once only held work remains.
- **Reset.** A core or port reset aborts held transactions with SLVERR. FATAL_STATUS and the hold clear only on a cold reset.

## 6. Error conditions without injection

| Condition | Firmware observes |
|---|---|
| Address outside every region of the port | DECERR, IRQ_RAW[4] |
| Region offset at or beyond USABLE_GRANULES × 32 | DECERR, no IRQ |
| Malformed AXI burst (size, burst type, WRAP length/alignment, FIXED length, 4 KiB crossing, region span) | SLVERR, memory unchanged |
| Exclusive write without a matching reservation | OKAY, no write |
| Illegal commit image | SLVERR; *_COMMIT_STATUS error with blocker; COMMAND_ERROR[2]; IRQ_RAW[4] and [10] |
| Command while another one holds the mutex | SLVERR; COMMAND_ERROR[0]; IRQ_RAW[10] |
| INIT_RW write during initialization or runtime training | SLVERR; COMMAND_ERROR[2] |
| Drain longer than POWER_TIMEOUT on a low-power entry | PDENY; POWER_ERROR pre-cutoff timeout *(prov.)*; IRQ_RAW[8] |
| Port access while its AXI domain is in reset | SLVERR and a simulation warning (integration error) |

## 7. Injection from the `fx1_soc` command line

`--ddr3-fault T_US:KIND[:ARG]` (repeatable) applies a fault T_US microseconds of simulated time after the CPU release; in firmware boot the release is time 0. Addresses are global DDR addresses; the conversion to a user granule assumes the bootstrap map (limitations L-21).

| KIND | API call | CPU test |
|---|---|---|
| `ce:ADDR` | `fault_flip_data_bit(granule, byte × 8)` | `ddr3_fault_recovery` B, `ddr3_preinit` C |
| `ue:ADDR` | two `fault_flip_data_bit` calls in the 8-byte lane of ADDR | `ddr3_fault_recovery` C, `ddr3_preinit` D |
| `check:ADDR` | `fault_flip_check_bit(granule, lane, 0)` | — (IP: `ecc ecc`) |
| `read-timeout[:N]`, `read-malformed[:N]`, `read-context[:N]`, `read-generation[:N]`, `read-unexpected` | `fault_read_return(...)` | `ddr3_fault_recovery` D (timeout) |
| `write-result[:N]` | `fault_write_result(N)` | — (IP: `ecc fatal`, `fatal_noecc`) |
| `fatal:dfi\|wdata\|unmatched\|stale\|scrub` | `fault_fatal(...)` | `ddr3_fault_recovery` F (`dfi`), `ddr3_boot_scrub_fatal` (`scrub`) |
| `refresh-block:R`, `refresh-unblock:R` | `fault_block_refresh(R, ...)` | — (IP: `lifecycle refresh`) |
| `phy-fail`, `phy-recover` | `functional_phy()->force_failure(...)` | `ddr3_boot_init_failure` |

Each application prints `[fx1_soc] ddr3 fault at <time>: <option>`. Recovery that needs a reset uses DDR_SYSCTRL RST_CTRL (programming guide §7.2, §7.3).
