# DDR3 Controller VP — Acceptance on `fx1_soc` (Subtask 5)

Audience: reviewers and the receiving FW/SW team. This report shows that the DDR3 controller VP is the DDR of `fx1_soc` in both boot modes, that firmware brings it up and uses it on the CPU, and that the existing CPU, DMA, ISP, atomic and FreeRTOS workloads still pass on it.

Baseline: CDC-VP branch `dev` (uncommitted working tree), Release build, `/usr/bin/g++` 11.5 with SystemC 2.3.4, xPack `riscv-none-elf-gcc`. "Former RAM" figures come from a build of the same tree before subtask 5 (`build-rel`, 2026-10-12), where DDR was a 10 ns `SparseRam`.

## 1. What was integrated

| Item | Where |
|---|---|
| DDR3 controller (NATIVE_R2, DDR3-1600, 2 ranks of 4 Gb x8 on x32, 4 GiB physical, ECC), bridge, CSR multiplexer, DDR_SYSCTRL, boot modes, host image/fault interface | `platforms/fx1_soc/src/ddr3_subsystem.*`, `ddr_sysctrl.*`, `fx1_soc_top.cpp`, `main.cpp` |
| Map: DDR CSR 0x1200_0000, DDR_SYSCTRL 0x1201_0000, boot SRAM 0x0100_0000 (1 MiB), DDR IRQ = PLIC source 8, `FX1_PLIC_NUM_SOURCES` 9, overlap/overflow assertions | `fx1/fx1_memory_map.h`, `fx1/fx1_ddr_sysctrl.h`, `bus/src/config.cpp` |
| Firmware: DDR3 bring-up helpers and the DDR3-1600 reference image, BootROM start-up and linker script, BootROM loader | `fw/fx1_soc/ddr3/`, `fw/fx1_soc/common/crt0_rom.S`, `fx1_rom.ld.S`, `fw/fx1_soc/boot/` |
| CPU tests, platform unit tests, CI manifest | `fw/fx1_soc/tests/ddr3/`, `platforms/fx1_soc/tests/test_ddr3_subsystem.cpp`, `platforms/fx1_soc/ci/required_tests.txt` |

The former `SparseRam` DDR is gone. CPU, SYS_DMA and ISP traffic reaches the controller through `GenericAxiBridge`. ELF preload, `--load` and `--dump` use the controller's debug port, which keeps the ECC records consistent. Nothing bypasses the controller on the normal access path.

## 2. Completion criteria (DDR3_VP_SUBTASKS.md, subtask 5)

| Criterion | Evidence |
|---|---|
| CPU boots and accesses DDR through the model | Preinit: every smoke image runs from DDR. `ddr3_preinit` checks that the active images equal the firmware reference. Firmware-init: `ddr3_fw_init`, `ddr3_boot_hello` and `ddr3_boot_freertos_cli` initialize the controller from BootROM, then use DDR |
| Init, IRQ and error paths verified on the CPU | `ddr3_fw_init`: commit reject (SLVERR, blocker 0x04, IRQ_RAW[4]); region decode miss → load access fault plus PLIC source 8. `ddr3_fault_recovery`: CE, UE, read timeout → quarantine → cold reset → re-initialization; fatal hold → core reset by hart 1 → cold reset. `ddr3_preinit`: CE/UE with PLIC interrupts. Boot failures (`ddr3_boot_ready_timeout`, `_init_failure`, `_scrub_fatal`) are reported from BootROM/SRAM |
| DMA/ISP/atomic regression and FreeRTOS pass | All 47 pre-existing smoke tests and the 6 `fx1_regression` tests pass, both quanta. The full-frame ISP demo (2688×1520 D65) matches the stand-alone model bit for bit (§5) |
| No former RAM bypassing the controller | §1; `fx1_ddr3_subsystem_preinit` reads the preloaded image back over the bus with clean ECC |
| Both boot modes, CI and hand-off documents | §3, §4; [integration_guide.md](integration_guide.md) §9, [programming_guide.md](programming_guide.md) §9, platform README and FW_GETTING_STARTED |

## 3. Boot modes

### 3.1 Preinitialized (default, `--ddr-boot preinit`)

Order (plan §5.3) as executed by the platform, with the test that proves each step:

1. Sideband at power-up: bootstrap region port 0 = 0x8000_0000/512 MiB, QoS configured (`fx1_ddr3_subsystem_sysctrl`).
2. The programming-guide CSR sequence through the same validation and lifecycle as firmware: timing, DFI, INIT, topology, three commits, `GLOBAL_CTRL.enable`. The installed images equal the firmware reference (`fx1_ddr3_subsystem_refimage`, `ddr3_preinit` A).
3. Mission readiness after the ECC scrub of 4 GiB: release at 672.09 ms of simulated time, a few ms of host time (`fx1_ddr3_subsystem_preinit`).
4. Preload of ELF DDR segments and `--load` files with `debug_write`, after the scrub (the same test; mutant "preload before readiness" caught).
5. CPU release. A BootROM entry runs only now: it sees readiness, a start time above 600 ms and the `--load` file in DDR (`ddr3_release_order`; mutant "release gate does not wait" caught). Time budgets, console replay and fault times count from here.

### 3.2 Firmware init (`--ddr-boot firmware`)

Plan §5.2, as implemented by the BootROM loader (`fw/fx1_soc/boot/boot_loader.c`) and checked by `ddr3_fw_init`:

1. Code in BootROM, data and stacks in SRAM (`fx1_rom.ld` fails to link anything into DDR). Hart 1 waits in WFI on MSIP plus a flag in SRAM. SYS_DMA and the ISP stay idle.
2. Firmware reads identity and capability, configures the QoS sideband through DDR_SYSCTRL, writes the timing, DFI, INIT_RW and topology images, commits them and sets `GLOBAL_CTRL.enable`.
3. It waits for mission readiness (DDR_SYSCTRL STATUS; 671.8 ms including the scrub) with a timeout. A timeout, an initialization failure or a fatal event is reported over the UART and sim-control from BootROM/SRAM (codes 0xD304/0xD305/0xD306, three negative tests).
4. The loader copies the application ELF from BootROM to DDR, issues `fence.i` and releases hart 1. The application zeroes its own `.bss` and sets up its stacks.
5. The application runs: `hello_2hart`, or the FreeRTOS CLI with a full scripted session (`ddr3_boot_freertos_cli`). BootROM use of the 2 MiB: 11.5 KiB for the loader with `hello_2hart`, 44.6 KiB with the FreeRTOS CLI (an image that does not fit fails to link).

An image with DDR segments is refused in this mode (`ddr3_boot_ddr_image_refused`), so nothing is loaded into DDR behind the firmware's back.

## 4. Test results

| Run | Result |
|---|---|
| `platforms/fx1_soc/ci/run_ci.sh` (configure, manifest, build, smoke, components, regression) | **PASS** |
| `ctest -L "fx1_unit\|fx1_smoke\|fx1_regression"` | 196/196 |
| of which DDR3 IP (`ddr3` label, without the platform cases) | 95/95 |
| of which platform unit tests `fx1_ddr3_subsystem_*` | 10/10 |
| of which DDR3 CPU tests (`fx1_smoke_ddr3_*`) | 17/17 |
| `fx1_exclusive_monitor` (now with region-alias identity and invalidation) | 1/1 |
| Components stage: bus tests, `uart2_tlm`, `uart_host_bridge`, `uart_host_bridge_hold` | 6/6 |
| Standalone DDR3 component `make test` | 101/101 |
| SDK distribution gate `run_packaging_regression.sh` (label `fx1_packaging`, outside `run_ci.sh`): fresh build, relocated package, empty-environment smoke and ISP golden, byte-identical firmware rebuild, interactive flow | **PASS**. The SDK now also ships `include/fx1_ddr3/` (register map, provisional encodings) and `include/fx1/fx1_ddr_sysctrl.h` |
| `fx1_all` build | 0 compiler warnings |

New CPU tests:

| Test | Boot | Checks |
|---|---|---|
| `ddr3_fw_init_q1000ns`, `_q10000ns` | firmware | Identity and reset state; INIT_CMD SLVERR; illegal CL → TIMING_COMMIT SLVERR, blocker 0x04, IRQ_RAW[4]; recommit; readiness; word/half/byte, aperture ends, scrubbed zero, no CE/UE; 256 MiB region → access above it = load access fault + IRQ_RAW[4] through PLIC 8; full region again with data kept; self-refresh → access → automatic wake; both harts with data checks and an AMO counter in DDR |
| `ddr3_fault_recovery` | firmware | Host schedule CE 1000 ms, UE 1010 ms, read timeout 1020 ms, DFI fatal 2000 ms: corrected data + CE_COUNT + ECC_LAST_* + IRQ_RAW[0]; UE load fault + UE_COUNT + IRQ_RAW[1]; timeout load fault + QUARANTINE_STATUS + IRQ_RAW[2], reads keep failing; cold reset clears quarantine and QoS, re-initialization, scrubbed and usable memory; fatal hold: hart 1 waits for the held read (STATUS.fe_drained = 0), resets the core, the held read faults on hart 0, FATAL_STATUS survives, a second cold reset clears it, re-initialization |
| `ddr3_preinit_q1000ns`, `_q10000ns` | preinit | STATUS (preinit, ready), GLOBAL_CTRL, INIT_STATUS, TOPO/TIMING_ACTIVE = reference; two-hart traffic; CE and UE from the host schedule with PLIC interrupts; self-refresh requested while executing from DDR, woken by the next fetch |
| `ddr3_release_order` | preinit | BootROM entry starts after readiness, after 600 ms, with the `--load` file in DDR |
| `ddr3_boot_hello`, `ddr3_boot_freertos_cli` | firmware | BootROM loader + application |
| `ddr3_boot_ready_timeout`, `_init_failure`, `_scrub_fatal`, `_ddr_image_refused` | firmware | Failure reporting from BootROM/SRAM; refusal of a DDR image |
| `ddr3_power_recovery` | firmware | Self-refresh, POWER_TIMEOUT = 1, DDR read: the wake fails, the automatic wake is denied and retried with back-off (PWR_STATUS[3], PACTIVE, POWER_STATUS[5]); hart 1 cold-resets, the held read faults on hart 0, re-initialization (review S5-R1) |
| `ddr3_wake_watchdog` | firmware | The same failure with no recovery agent ends with RESULT TIMEOUT at the `--timeout-ms` budget (exit 2): simulated time keeps advancing (review S5-R1) |
| `ddr3_atomic_lifecycle` | firmware | LR, then cold reset / core reset / CSR-driven reinitialization / region commit: the SC fails and writes nothing (review S5-R2) |
| `ddr3_atomic_alias_q1000ns`, `_q10000ns` | preinit | Two aliased regions: a hart store and a DMA write through one name void the reservation taken through the other; amoor.w through one name against DMA writes through the other loses no write, and the DMA writes really wait for the AMO brackets (review S5-R3) |

### 4.1 Mutation checks

| Area | Mutants | Caught |
|---|---|---|
| DDR_SYSCTRL and subsystem: automatic wake removed, entries not counted, QOS_CFG ignored, STATUS IRQ bit dropped, core reset without reset-safe wait, cold reset pulse missing, preload before readiness, release before the scrub, DDR staging accepted in firmware boot | 9 | 9 (the reset-safe case only after adding a launched read in flight) |
| Platform, with the CPU tests: release gate not waiting, AUTO_WAKE reset 0, fault times absolute, UART input not held, DDR IRQ not routed to PLIC 8 | 5 | 5 (the release gate only after adding `ddr3_release_order`) |
| `uart_host_bridge` hold ignored | 1 | 1 (after replacing `assert`, which compiled to nothing in Release, with checks) |
| Review fixes S5-R1..R3: wake retry without back-off; storage listener not wired; no hook at initialization restart/scrub start; no hook at region install; canonicalizer not wired; write ranges, brackets or SC address not canonical; no split at a region boundary; no split at the next region after a hole | 10 | 10 (the two split mutants only after adding `fx1_ddr3_subsystem_canonical`) |

## 5. Workloads and cost

Simulated time is counted from CPU start. Host time and peak RSS are for one run on the development host and vary with host load.

| Workload | Boot | Result (former RAM) | Simulated after CPU start, ms (former RAM) | Host time, s (former RAM) | Host × | Peak RSS, MiB |
|---|---|---|---|---|---|---|
| hello_2hart | preinit | PASS (PASS) | 0.09 (0.05) | 0.00 (0.00) | — | 10.6 |
| perf_loop | preinit | PASS (PASS) | 23.33 (9.40) | 0.59 (0.14) | 4.2 | 10.4 |
| dma_m2m | preinit | PASS (PASS) | 1.31 (0.58) | 0.03 (0.01) | — | 10.7 |
| lrsc_masters | preinit | PASS (PASS) | 12.78 (4.67) | 0.32 (0.09) | 3.6 | 10.9 |
| amo_dma | preinit | PASS (PASS) | 1.94 (0.85) | 0.05 (0.01) | — | 10.6 |
| spinlock_2hart | preinit | PASS (PASS) | 276.38 (112.68) | 7.32 (2.03) | 3.6 | 10.5 |
| shared_mem_concurrent | preinit | PASS (PASS) | 97.01 (30.63) | 2.21 (0.53) | 4.2 | 10.7 |
| isp_stream_basic | preinit | PASS (PASS) | 57.52 (27.19) | 1.04 (0.23) | 4.5 | 10.9 |
| isp_recovery ¹ | preinit | PASS (PASS) | 320.83 (148.88) | 5.48 (1.23) | 4.5 | 10.9 |
| isp_mas0710 ¹ | preinit | PASS (PASS) | 3887.23 (1804.66) | 79.49 (17.08) | 4.7 | 13.7 |
| freertos_cli scripted session ² | preinit | PASS (PASS) | 406.91 (143.62) | 5.60 (1.16) | 4.8 | 10.6 |
| ISP demo, one 2688×1520 frame (`run_isp_demo.py`) | preinit | MATCH (MATCH) | 12.82 (8.22) first queue to last completion | 4.02 (3.79) incl. the reference model | — | — |
| ddr3_preinit | preinit | PASS | 21.16 | 0.09 | — | 10.7 |
| ddr3_fw_init | firmware | PASS | 672.62 | 0.05 | — | 10.5 |
| ddr3_fault_recovery | firmware | PASS | 2671.88 (three initializations) | 2.09 | — | 16.6 |
| boot_hello | firmware | PASS | 672.36 | 0.04 | — | 10.5 |
| boot_freertos CLI session | firmware | PASS | 1346.92 | 5.73 | — | 11.1 |

¹ With the DDR3, the abort phases use 2-beat ISP bursts (VPL-49); the former-RAM run uses 16. ² With the DDR3, the replay is paced at 87 µs per byte from 60 ms (VPL-49); the former-RAM run used 10 µs from 20 ms.

Preinit adds 672 ms of simulated time before CPU start (initialization and the ECC scrub of 4 GiB) at a cost of a few milliseconds of host time; the time budgets exclude it. The CPU-bound workloads take about 2–2.5× more simulated time and about 4–5× more host time. The CPU fetches every instruction from the DDR3 model: about 36 ns per fetch instead of 19 ns. A fetch queued behind a DMA burst on the serialized bus waits for that burst. The host cost is about 1 µs per DDR access, mostly SystemC process switches. Identical-behaviour optimizations cut it by 17 % (VPL-56). Memory stays near 11 MiB: the sparse store grows only with written data.

## 6. Adjusted regression stimulus (VPL-49)

Four existing tests encoded timing assumptions that the DDR3 model invalidates. Their stimulus was adjusted (user decision of 2026-10-15); no check was weakened.

| Test | Cause (measured) | Adjustment |
|---|---|---|
| `freertos_cli_q1000ns`, `_q10000ns`, `_bad_input` (and the SDK `run_vp.sh`) | Boot now takes about 26 ms, so a replay at 20 ms arrives before the console reads. At 10 µs per byte, a CLI line printed with interrupts masked outlasts the 16-entry RX FIFO (input lost; ≥ 40 µs per byte passes) | Replay from 60 ms at 87 µs per byte (115200 8N1), new option `--uart0-rx-byte-us` |
| `isp_recovery` (phases C/D/E), `isp_mas0710` (stream with phase B), both quanta | The CPU must reach the abort point within one 64×48 frame (about 16 µs of IDMA activity). Behind 16-beat ECC write bursts it needs 18.5 µs; with the former RAM it needed 2 µs | ISP DMA bursts of 2 beats in those phases (`ISP_ABORT_MAX_BURST_BEATS`): abort after 12 µs, input read 31–37 µs. Data and completion order are unchanged |

One component test was updated as well. `bus_config` "enabled TBD" used the SRAM slot, which now has its placeholder address; it uses CPU1_IRAM instead, and a new check validates the three new placeholders together.

## 7. Review follow-up (S5-R1..R3)

| Finding | Fix | Regression |
|---|---|---|
| S5-R1 a denied automatic wake looped at one instant (kernel stopped, `--timeout-ms` and resets ineffective) | DDR_SYSCTRL retries a denied automatic wake after a back-off in simulated time (1 µs doubling to 1 ms), reports PWR_STATUS[3], serves RST_CTRL/PWR_* meanwhile (VPL-58) | `fx1_ddr3_subsystem_wake_recovery` (PHY not ready → recovery required), `fx1_ddr3_subsystem_wake_busy` (mutex owner), `ddr3_power_recovery`, `ddr3_wake_watchdog`; the reviewer's CPU probe now ends with RESULT TIMEOUT, the subsystem probe passes |
| S5-R2 a CPU reservation survived a cold reset and the scrub | Controller `set_storage_listener` (core/cold reset, initialization restart, scrub start, region install) drops the DDR reservations of the platform monitor (VPL-59) | `ddr3_atomic_lifecycle`; the reviewer's probe now fails the SC and keeps 0 |
| S5-R3 the CPU monitor did not see region aliases | Canonical storage names for all monitor participants from the effective map (VPL-60) | `fx1_exclusive_monitor` (aliases), `fx1_ddr3_subsystem_canonical`, `ddr3_atomic_alias`; the reviewer's probe now fails the SC and keeps hart 1's data |

Host cost of the canonical lookup on every CPU store: `perf_loop` 0.59–0.62 s (0.59 s before), identical simulated time.

## 8. Limits of this acceptance

- **CPU timing** is pessimistic compared with silicon: no cache, serialized DDR traffic ([limitations.md](limitations.md) L-19). DDR_SYSCTRL is a VP placeholder (L-20). Host address conversion for dump and faults assumes the bootstrap map (L-21).
- **Sanitizers**: not run, because libasan and libubsan are not installed.
- **Encodings** that the specification leaves open remain provisional (DEC-13). Firmware uses the macros.

## 9. Reproduce

```bash
cd CDC-VP
platforms/fx1_soc/ci/run_ci.sh --build-dir build-fx1-ci --jobs 6          # the acceptance gate
build-fx1-ci/platforms/fx1_soc/fx1_soc --fw build-fx1-ci/fw/fx1_soc/ddr3_fw_init.elf \
    --ddr-boot firmware --timeout-ms 3000                                 # firmware-init on the CPU
build-fx1-ci/platforms/fx1_soc/fx1_soc --fw build-fx1-ci/fw/fx1_soc/freertos_cli.elf \
    --uart0-rx-file fw/fx1_soc/freertos/tests/cli_smoke.txt \
    --uart0-rx-delay-us 60000 --uart0-rx-byte-us 87 --timeout-ms 3000     # preinit + FreeRTOS
platforms/fx1_soc/package/run_packaging_regression.sh                     # SDK distribution gate
```
