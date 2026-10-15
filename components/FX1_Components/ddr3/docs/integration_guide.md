# DDR3 Controller VP — Integration Guide

Audience: platform integrators who instantiate the model in a SystemC/TLM virtual platform (FX1 `fx1_soc` or a standalone bench). This guide covers instantiation, binding, sideband control, the preinitialized boot mode, preload/dump and the host API. The binding contracts in full are in [interfaces.md](interfaces.md); firmware-visible behaviour is in [programming_guide.md](programming_guide.md).

## 1. Build and link

| Use | How |
|---|---|
| Inside CDC-VP | `add_subdirectory(ddr3)` already exists in `components/FX1_Components`. Link `cdc::components::fx1_ddr3` |
| Standalone package | `make test` in the component, then `cmake --install build --prefix <dir>`. Consumers use `find_package(fx1-ddr3 0.1)` (with `<dir>` on `CMAKE_PREFIX_PATH` or `-Dfx1-ddr3_DIR=<dir>/lib/cmake/fx1-ddr3`) and link `cdc::components::fx1_ddr3`. `tests/consumer/` is the reference consumer; the `fx1_ddr3_package_consumer_*` tests build and run it once per SystemC discovery method |
| Pure C++ only (no SystemC) | `cdc::components::fx1_ddr3_core`: profiles, speed bins, config checks, CSR map, frontend decode, mapping, ECC codec, bank timer, sparse store, exclusive monitor |

Requirements: C++17 and SystemC 2.3.4. On the FX1 hosts, use `/usr/bin/g++` and `-DSYSTEMC_HOME=/opt/systemc-2.3.4`.

**Locating SystemC.** The source build and the installed package use the same discovery (`cmake/fx1-ddr3-find-systemc.cmake`, installed with the package). Methods, in priority order:

1. An existing `SystemC::systemc` target, for example from a parent project, is reused.
2. `SYSTEMC_HOME`, given as a CMake variable or else from the environment, is searched exclusively. Its SystemCLanguage package is tried first; if the installation has no CMake metadata, its header and library are used.
3. Otherwise the default search runs: a SystemCLanguage package on `CMAKE_PREFIX_PATH`, then a header and library search on the same paths. `SYSTEMC_INCLUDE_DIR` and `SYSTEMC_LIBRARY` may also be set directly.

The consumer regression covers five cases, each in a clean build directory:

- SystemC prefix on `CMAKE_PREFIX_PATH`;
- `-DSYSTEMC_HOME`;
- environment `SYSTEMC_HOME`;
- a header/library-only prefix given as `SYSTEMC_HOME`;
- the same header/library-only prefix on `CMAKE_PREFIX_PATH`;
- explicit `SYSTEMC_INCLUDE_DIR` and `SYSTEMC_LIBRARY`.

The fixture is derived from the SystemC that the producer build resolved, so the cases hold whichever method configured the producer.

On the FX1 hosts the login environment may export a different `SYSTEMC_HOME` (Fast Models). Pass `-DSYSTEMC_HOME=/opt/systemc-2.3.4` explicitly: the CMake variable has priority over the environment.

## 2. Instantiation

```cpp
#include "fx1_ddr3/ddr3_controller.h"
#include "fx1_ddr3/generic_axi_bridge.h"

fx1::ddr3::BuildConfig cfg = fx1::ddr3::BuildConfig::native_r2();  // native_r1(), legacy()
cfg.n_ports = 1;                 // DDR_SYS_N_PORT; only these sockets may be bound
cfg.ecc_enable = true;           // DDR_SYS_ECC_ENABLE
cfg.speed_bin = fx1::ddr3::SpeedBin::DDR3_1600;
fx1::ddr3::Ddr3Controller ddr("ddr3", cfg);       // throws std::invalid_argument on an illegal build
fx1::ddr3::GenericAxiBridge bridge("ddr3_bridge", cfg.axi_beat_bytes());
```

`BuildConfig` fields mirror the build options in MAS Tables 5-4 to 5-8: profile, ports, ID width, ECC, speed bin, device width and density, map-profile mask, row bits, IRQ test, exclusive enable and port mask, refresh debt limit, and the PHY latencies. `validate(cfg)` lists every problem in a configuration.

The controller owns a `FunctionalPhy` unless a `PhyProvider*` is passed as the third constructor argument (§6).

## 3. Binding

| Socket / port | Bind to | Notes |
|---|---|---|
| `csr_socket` (target) | The bus target of the CSR aperture (FX1: `DDR3_CSR` at 0x1200_0000, 64 KiB) | The bus must deliver 16-bit offsets (aperture base removed), 4-byte accesses |
| `mem_socket[p]` (target, p < n_ports) | `GenericAxiBridge::initiator`, or an AXI-aware initiator | Every payload must carry `AxiExtension` (interfaces.md §2). Binding a socket ≥ n_ports is an elaboration error |
| `bridge.target` | The bus target of the DDR aperture (FX1: `MEMCTL_DDR`, 0x8000_0000 / 512 MiB) | `address_offset` (constructor) converts a bus-local address to the global AXI address if the bus translates. The FX1 bus always removes the target base, so `fx1_soc` uses `address_offset = 0x8000_0000` (VPL-53) |
| `irq` (`sc_out<bool>`) | Interrupt controller input (FX1: PLIC source 8) | Must be bound |
| `mission_ready_out`, `global_idle_out`, `fe_drained_out`, `init_complete_out` | Optional `sc_signal<bool>` | Mirrored by DDR_SYSCTRL (DEC-04). Getters return the same values |

**Delay handling.** The controller consumes the incoming annotated delay at entry and returns with `delay = 0`. The FX1 bus already accounts for its own delays, so no delay is counted twice.

**DMI** is not offered. Every access goes through admission, ECC, fault handling and reset.

## 4. Sideband controls (DDR_SYSCTRL, or the bench)

| API | When |
|---|---|
| `set_bootstrap_region(port, region, {base, size})` | Before simulation (or before the first access). Decode map until firmware commits a region image, and again after a core reset |
| `qos_write(port, index, cls)`, `qos_set_configured(port, true)` | Before any traffic on that port, and after every port soft reset (it clears the table). Without it the port stalls every request (FE05) |
| `set_reserved_rate(port, rate, depth)` | Optional; stored only (L-15) |
| `pchannel_request(preq, pstate)`, `pchannel()`, `pchannel_event()` | System power controller. 4-phase: raise PREQ with PSTATE, wait for PACCEPT or PDENY, drop PREQ, wait for both to fall |
| `set_cold_reset / set_core_reset / set_csr_reset / set_port_reset(p, …)` | Reset controller; `true` = asserted |
| `core_reset_request`, `core_reset_safe()`, `csr_reset_request`, `csr_reset_safe()` | Coordinated resets (HAS §7.1) |
| `port_quiesce_request(p, …)`, `port_quiesce_ack(p)`, `port_soft_reset_request(p, …)` | Per-port lifecycle (HAS §6.1.14) |
| `lifecycle_event()` | Notified when any acknowledgement or readiness level changes |

For FX1 port 0, the platform sets the bootstrap region `{0x8000_0000, 0x2000_0000}` and configures the QoS table at elaboration, so firmware boot and preinit both find the port open. Firmware reaches the sideband through DDR_SYSCTRL (§9.2); after a cold reset it configures the QoS table again there.

## 5. Boot modes

### 5.1 Firmware init

The CPU runs the sequence in [programming_guide.md](programming_guide.md) §4 from ROM/SRAM. The platform only provides §4 of this guide. Code and data must stay outside DDR until readiness: with ECC, the startup scrub zeroes DDR.

### 5.2 Preinitialized (`preinit`, DEC-09)

The platform brings the controller up before releasing the CPUs. It uses the same CSR programming as firmware, issued from a host thread through a TLM initiator bound to `csr_socket`, or through the bus. It then preloads the program image (`fx1_soc` does exactly this, §9.1):

1. Configure the sideband (§4).
2. Write the timing, DFI, INIT_RW and TOPO words, and commit topology, timing and DFI. `fx1::ddr3::timing_image(cfg)`, `dfi_image(cfg)` and `init_wait_image(cfg)` return legal values for the build. The `program_and_commit()` helper in `tests/ddr3_bench.h` is a working example.
3. Write `GLOBAL_CTRL.enable = 1`, then wait for `mission_ready()`, e.g. on `lifecycle_event()`. With ECC, the scrub of a 4 GiB configuration takes about 0.67 s of simulated time and microseconds of wall time (one scheduled event).
4. Preload with `ddr.debug_write(user_offset, data, len)`, or with `transport_dbg` on the bridge or a memory socket using global addresses. Preload **after** readiness: data written before the scrub frontier passes it is erased, and the model warns.
5. Release the CPUs.

Log mode, profile, ECC and aperture at the start of the run (plan §5.3).

Release the CPUs in a way that holds **every** bus access of the harts, not only DDR ones: a BootROM entry would otherwise run during the platform's CSR sequence. For DDR images the controller's own gate is enough (an access waits at the backend until readiness, and the preload is written in the same instant as readiness, before any read data is taken), but a release gate covers both cases.

## 6. PHY provider

`PhyProvider` (`phy_provider.h`) is the functional PHY peer:

- the controller drives reset_n, clock_disable and init_complete;
- it issues one training load at a time;
- it receives tagged observations and readiness levels.

`FunctionalPhy` acknowledges loads after a configurable latency. It can also script failures: Fail, Lose, StaleEpoch, WrongRank, WrongTag and Delay per load, plus forced failure or readiness loss. A platform PHY model can replace it by implementing `PhyProvider`. Its observations must echo the request's rank, epoch and tag.

## 7. Host debug and fault API

These calls are for benches and the platform CLI. Firmware cannot reach them. All addresses are user (region-normalized) byte offsets unless noted.

| Call | Purpose |
|---|---|
| `decode_map(port)` | Effective region map (committed image or bootstrap map): a global address in entry r has user offset `addr - base` |
| `set_storage_listener(fn)` | `fn()` is called synchronously when a core or cold reset, an initialization (re)start, the start of the scrub or a region install changes what DDR addresses hold or designate. A platform exclusive monitor drops its DDR reservations there |
| `debug_write` / `debug_read` | Preload and dump. Writes regenerate ECC records; reads return stored bytes uncorrected; no interrupt or admission effect |
| `resident_pages()` | Host memory in use (4 KiB pages) |
| `fault_*` | See [fault_catalogue.md](fault_catalogue.md) |
| `ce_count()`, `ue_count()`, `quarantined()`, `fatal()`, `fatal_status()` | Error observation |
| `mission_ready()`, `init_complete()`, `init_stage()`, `init_failed()`, `training_complete()`, `scrub_complete()`, `scrub_running()`, `scrub_progress()`, `scrub_total()`, `power_resident()`, `recovery_required()`, `refresh_debt(rank)` | Lifecycle observation |
| `geometry()`, `bank_timer()`, `scheduler()` | Committed geometry, row hit/miss/conflict counters, scheduled-action counters |

## 8. Performance notes

- **Idle cost.** An idle controller schedules no periodic action: refresh, readiness and timeouts are evaluated lazily. The `verify idle_events` test runs 100 ms of idle time with zero actions.
- **Storage.** Storage grows only with written data plus its ECC records, in 4 KiB pages. The startup scrub drops pages; it never allocates any. Reference run (`verify perf`): a 4 GiB physical configuration with ECC and 8 MiB of traffic holds 2305 pages, about 18 MiB RSS for the whole process.
- **Simulated latency** is approximate (L-02). Bus-level time budgets must not rely on cycle-exact values.

## 9. FX1 integration (`fx1_soc`)

Source: `platforms/fx1_soc/src/ddr3_subsystem.{h,cpp}`, `ddr_sysctrl.{h,cpp}`, `fx1_soc_top.cpp`. The subsystem replaces the former `SparseRam` DDR: every CPU, DMA and ISP access to the DDR aperture goes through the controller.

| Part | FX1 binding |
|---|---|
| Controller | NATIVE_R2, DDR3-1600, 4 Gb x8 on x32, 2 ranks (4 GiB physical), ECC on (`--ddr3-ecc off` for experiments), one memory port |
| Data | `MEMCTL_DDR` → `GenericAxiBridge(address_offset 0x8000_0000)` → `mem_socket[0]` |
| CSR | `DDR3_CSR` 0x1200_0000 → CSR multiplexer (bus + platform preinit initiator) → `csr_socket` |
| DDR_SYSCTRL | 0x1201_0000 (`fx1/fx1_ddr_sysctrl.h`) |
| Interrupt | `irq` → PLIC source 8 (`FX1_IRQ_DDR`, `FX1_PLIC_NUM_SOURCES` = 9) |
| Boot SRAM | 0x0100_0000 / 1 MiB, enabled for firmware-init images |
| CPU release | A gate between each hart's port adapter and its bus port |

### 9.1 Boot modes on the command line

| Option | Effect |
|---|---|
| `--ddr-boot preinit` (default) | §5.2. The simulation runs the initialization and the 0.67 s ECC scrub, preloads the ELF's DDR segments and every `--load` file, then releases the harts. `--timeout-ms`, `--uart0-rx-delay-us` and `--ddr3-fault` times count from the release, so existing budgets keep their meaning |
| `--ddr-boot firmware` | §5.1. The harts start at time 0 from BootROM. An ELF with DDR segments, or a `--load` into DDR, is an error (exit status 3): nothing could be initialized before the scrub erases it |
| `--ddr3-fault T_US:KIND[:ARG]` | Host fault injection at T_US after the release; kinds as in `fx1_soc --help` and [fault_catalogue.md](fault_catalogue.md) |
| `--dump ADDR:LEN:FILE` | `debug_read` of stored bytes (no ECC correction) after the run |

The run log starts with one line naming the boot mode, profile, ECC, ranks and aperture, and in preinit mode a second line with the release time. The last lines include DDR3 counters (AXI bursts, row hits/misses/conflicts, CE/UE, quarantine, fatal status, power entries, resets).

### 9.2 DDR_SYSCTRL

| Offset | Register | Purpose |
|---|---|---|
| 0x000 | ID | 0x4444_5343 |
| 0x004 | STATUS | mission_ready, init_complete, global_idle, fe_drained, irq; [8] preinit boot |
| 0x010 / 0x014 | PWR_REQ / PWR_STATUS | 4-phase P-channel request; busy, accepted, denied, automatic wake denied [3], last request, resident state, PACTIVE |
| 0x018 / 0x01C | PWR_CTRL / PWR_COUNT | AUTO_WAKE (reset 1): answer PACTIVE with an ACTIVE request; entry and automatic-wake counters |
| 0x020 / 0x024 | RST_CTRL / RST_STATUS | Cold reset, or coordinated core reset (request, reset-safe within 1 ms, pulse) |
| 0x040 + 4p | QOS_LUT(p) | 16 × 2-bit QoS classes of port p |
| 0x050 | QOS_CFG | bit p: port p configured (FE05) |
| 0x060 + 8p / 0x064 + 8p | RSV_RATE(p) / RSV_DEPTH(p) | Reserved-rate sideband (stored, L-15) |

Accesses are 32-bit aligned words; unmapped offsets answer DECERR, writes to read-only words SLVERR. DDR_SYSCTRL is a VP placeholder (L-20).

### 9.3 Firmware

`fw/fx1_soc/ddr3/` holds reference bring-up code (`ddr3_fw.h`, `ddr3_ref_1600.h`): the programming-guide sequence with the DDR3-1600 image, readiness waits, fault-safe accesses and the DDR_SYSCTRL sequences. It is example code for the tests and the BootROM loader, not a driver. `fw/fx1_soc/boot/boot_loader.c` with `common/crt0_rom.S` and `common/fx1_rom.ld.S` builds BootROM images (`fx1_rom_firmware()`, `fx1_boot_image()` in `fw/fx1_soc/CMakeLists.txt`).

### 9.4 Automatic wake and CPU atomics

- **Denied automatic wake.** A wake request can be denied for ever (recovery required after a failed wake, initialization failed) or for a while (a CSR command owns the mutex), while PACTIVE stays high. DDR_SYSCTRL retries after a back-off in simulated time, 1 µs doubling to 1 ms, sets PWR_STATUS[3] and keeps serving RST_CTRL and PWR_* meanwhile. A held access ends with a cold reset; a coordinated core reset cannot reach reset-safe while that access is owned work (RST_STATUS[2] after 1 ms).
- **One storage identity for atomics.** The platform exclusive monitor (LR/SC reservations, SC/AMO brackets, SYS_DMA and ISP ODMA write guards) compares DDR locations, not bus names: `Ddr3Subsystem::canonical_ranges()` names a decoded DDR address by its user offset (region aliases share one name, a range is split at region boundaries) and every storage change reported by `set_storage_listener` drops the DDR reservations. Tests: `fx1_ddr3_subsystem_canonical`, `ddr3_atomic_alias`, `ddr3_atomic_lifecycle`.

### 9.5 Cost

The CPU executes from DDR without a cache, through a bus that serializes DDR traffic: about 2× slower in simulated time, up to 9× during ISP streaming, and about 4–5× more host time per instruction than with the former RAM (L-19). Measurements per workload are in [acceptance_report.md](acceptance_report.md).
