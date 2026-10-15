# DDR3 Controller VP — Architecture

Audience: VP developers and integrators. Firmware-facing behaviour belongs in the
programming guide (subtask 4). Decisions are cited as `SPEC-xx` / `DEC-xx` ([decision_log.md](decision_log.md)).

## 1. Abstraction level

| Aspect | Modelled | Not modelled (reason) |
|---|---|---|
| CSR | Full HAS §8 map: decode, access classes, strobes, shadow/active images, commits, mutex, snapshots, IRQ; undefined encodings provisional (DEC-13) | 30-word snapshot packing (GAP-CSR-20) |
| AXI memory ports | One TLM call per AXI transaction with an AXI extension: legality, region decode, responses, same-ID order, exclusive verdict, data placement per lane, QoS admission gate | VALID/READY handshakes, CDC FIFOs, credits, per-cycle arbitration (below VP level) |
| Backend | Address mapping (linear/interleaved), per-rank/bank open-row state, DRAM timing and refresh windows applied to latency (DEC-02), refresh debt, ECC codec and companion storage, RMW lock, scrub, read timeout/quarantine, fatal containment | Cycle-level OoO scheduler, write-drain hysteresis, DFI phase/serialisation (abstracted; observable only through SCHED status words, which read 0 per DEC-03) |
| Lifecycle | Init stages, boot training (DEC-06), runtime training, power states via P-channel, quiesce, port soft reset, epoch-safe completions | Analog PHY behaviour, clock gating, scan |
| PHY | Functional provider: readiness levels, one acknowledged load at a time, tagged observations, scripted faults | DFI pins, eye search, electrical behaviour |
| Time | Event-driven; integer-picosecond arithmetic; no free-running clock thread while idle | Cycle accuracy, bandwidth fidelity |

## 2. Code structure

```text
include/fx1_ddr3/             public headers
  profile.h                   BuildConfig, profiles, build-time validation, capability words
  speed_bin.h                 JEDEC-derived images (MAS §5.5-5.6) for build params / legal images
  timebase.h                  tCK / controller-cycle conversion (ceil(n/R))
  axi_types.h                 AXI request/response vocabulary, generic-access burst planner
  axi_extension.h             TLM extension = memory-port transaction contract
  frontend.h                  region decode, burst legality, beat walk, atom (granule visit) plan
  address_map.h               granule -> rank/bank/row/column, companion granule and slot
  ecc_codec.h                 SECDED (72,64) per lane, granule record {E3..E0}
  bank_timer.h                approximate per-bank DRAM timing (open row, ACT/PRE/column spacing)
  granule_store.h             sparse store keyed by physical granule, range erase
  exclusive_monitor.h         reservations, allocation and clearing rules
  generic_axi_bridge.h        platform adapter: generic bus payload -> AXI transactions
  phy_provider.h              PHY peer contract (levels, load, observation)
  functional_phy.h            default PHY provider with fault script
  epoch_scheduler.h           timed actions with per-domain epochs (stale-completion safety)
  ddr3_controller.h           sc_module: sockets, ports, sideband API
src/                          implementation (ddr3_memory.cpp: the data path)
tests/                        core tests (no SystemC) and SystemC tests
docs/                         this document, interfaces, inventory, decisions, audits, traceability
```

Libraries:

- `fx1_ddr3_core` (`cdc::components::fx1_ddr3_core`) is pure C++17. It holds everything that
  can be decided without simulated time: profile legality, register semantics, mapping, ECC
  codec, timing legality. These are unit-tested without a SystemC kernel.
- `fx1_ddr3` (`cdc::components::fx1_ddr3`) is the SystemC/TLM layer: sockets, lifecycle
  sequencing in simulated time, PHY provider, scheduler.

## 3. Planned block decomposition (subtasks 2–3)

| Block | Library | Responsibility | Subtask |
|---|---|---|---|
| `CsrBank` | core | 210-word map, access classes, strobes, reserved bits, shadow/active images, validation | 2 |
| `CommitEngine` / mutex | tlm | Command serialization, acknowledged installs, blocker reporting | 2 |
| `IrqController` | core | 12 sticky sources, mask, W1C, hardware-set priority, test set | 2 |
| `Lifecycle` | tlm | Init stages, boot/runtime training, readiness composition, quiesce, power, P-channel | 2 |
| `RefreshModel` | core + tlm | Per-rank signed debt, urgency (BE04), deadlines evaluated lazily from timestamps | 2–3 |
| `Frontend` | core + tlm | Region decode, burst legality, contexts, same-ID ordering, exclusive monitor, QoS gate | 3 |
| `AddressMap` | core | Normalized address → rank/bank/row/column, companion granule and record slot | 3 |
| `EccCodec` | core | SECDED (72,64) per MAS §6.2.17, four codewords per granule | 3 |
| `MemoryService` | tlm | Sparse storage (reuses `fx1::SparseStorage` semantics), bank state, latency, RMW lock, scrub frontier, read timeout, quarantine, fault injection | 3 |
| `GenericAxiBridge` | tlm | Platform adapter: generic 32-bit bus payload → AXI transactions on port 0 | 3 |
| `DdrSysctrl` | platform | SoC glue at 0x12010000 (DEC-04) | 5 |

## 4. Timing model rules

- One owner per delay: the controller consumes the incoming annotated delay (`wait`) at entry,
  waits for its own modelled latency, and returns with `delay = 0`. The FX1 bus already
  consumes delays before and after the target, so no delay is counted twice.
- Waits counted in tCK use `ceil(n / R)` controller cycles; watchdogs count controller cycles
  (`TimeBase`). INIT_* waits count controller cycles (HAS, SOQ-24).
- Refresh, timeouts and scrub progress are evaluated from timestamps when observed or when a
  scheduled deadline fires. There is no per-cycle `SC_THREAD`.
- Scrub over U granules is one scheduled completion after N_total × per-write time, with a
  frontier so reads of not-yet-scrubbed space and abort/restart remain exact (DEC-10).
- Every scheduled completion belongs to an `EpochScheduler` domain. Reset, reinitialization
  and new episodes bump the epoch so stale completions are dropped, never delivered.
- No global mutex is held across `wait()`; CSR accesses stay serviceable while memory traffic
  is stalled.

## 5. Storage and ownership

- One storage owner: the controller. CPU, DMA, preload and dump all reach the same sparse
  store through the controller (plan §4.1). In `fx1_soc` the former `SparseRam` DDR is gone:
  `--load` and ELF preload use `debug_write` after the scrub, `--dump` uses `debug_read`.
- User data and hidden ECC companion granules share one physical store. ECC records are
  produced by the codec, so preload after scrub creates consistent ECC; deliberate corruption
  uses the fault API.
- Sparse pages are 4 KiB; a scrubbed range is represented by a frontier/zero-range set instead
  of allocated zero pages.

## 6. Status at the end of subtask 5

Subtask 5 integrated the IP into `fx1_soc` (`platforms/fx1_soc/src/ddr3_subsystem.*`):
controller, bridge, CSR multiplexer, DDR_SYSCTRL glue (readiness mirror, P-channel with
automatic wake, QoS sideband, cold/core reset sequencing), both boot modes with a CPU release
gate, the host fault CLI, BootROM/SRAM firmware images and a BootROM loader, and the CPU
acceptance tests. Results: [acceptance_report.md](acceptance_report.md).

## 7. Status at the end of subtask 4

Subtask 4 closed every remaining audited requirement with a test or an agreed limitation
([limitations.md](limitations.md)), added the conformance cases, seeded soaks and efficiency
checks (`test_verify`), a package consumer test, the PHY clock-disable control around
self-refresh, and the hand-off documents (programming guide, integration guide, fault
catalogue, verification report).

## 8. Status at the end of subtask 3

Subtask 3 added the memory data path (`ddr3_memory.cpp`) and its pure core blocks
(`frontend`, `address_map`, `ecc_codec`, `bank_timer`, `granule_store`, `exclusive_monitor`):

- Frontend: two admission levels (port gate, backend gate; VPL-18), 32 contexts per port,
  region decode on the effective map with the MAS Table 6-22 checks, same-ID and read-return
  order, exclusive monitor, sticky-forward responses, publication quiesce for region commits.
- Backend: request slots with generations, granule FIFO locks (install order, RMW protection),
  mapping with capacity poison, approximate bank timing, SECDED companion storage with full
  and partial (RMW) writes, CE/UE events with counters, last address/info and interrupts.
- Lifecycle links: startup-scrub data effect as a frontier, power entry and runtime training
  drain the backend, fatal containment closes readiness, read quarantine with the MAS
  first-cause record.
- Host API: debug preload/dump with consistent ECC, `transport_dbg`, fault injection (data
  and check bits, read returns, write results, fatal sources, scrub fault).
- Platform adapter `GenericAxiBridge`.

Known limitations (performance is not claimed): in-order bank model, so ECC traffic whose
companion granule shares the data bank shows row conflicts a reordering scheduler would hide;
a mandatory refresh urgency does not block issue by itself (REF windows do); storage is keyed by granule, so a geometry change does not
scramble contents (BE-6.2.2-19); regions alias the same user offsets (SOQ-20).

## 9. Status at the end of subtask 2

Subtask 2 added: the 210-word CSR map with every access class (`csr_map`, `ddr3_csr.cpp`),
validation of every configuration group (`config_checks`), the command mutex, acknowledged
commits with both blocker locations, coherent snapshots, the 12-source interrupt controller,
the initialization sequencer, boot and runtime training through the PHY provider, startup
scrub timing, readiness composition, lazy refresh debt (BE04) and the P-channel power
lifecycle (`ddr3_lifecycle.cpp`), and the C99 register header. The memory data path, ECC
codec, quarantine and the backend admission gate remain for subtask 3.

## 10. Status at the end of subtask 1

Implemented and tested: profile/build validation, capability words, JEDEC speed-bin images,
time base, generic-access burst planner, AXI extension contract, PHY provider contract and
functional PHY with fault script, epoch scheduler, controller skeleton (sockets, port-count
check, sideband storage, identity CSR words), reset/quiesce bundle handshakes (reset-safe,
port quiesce ack, soft-reset pending/hold, access-during-reset errors). Every other CSR or memory access returns an
explicit error and a one-time "not implemented" warning; no path fabricates success.
