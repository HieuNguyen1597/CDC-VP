# FX1 DDR3 Controller VP (`fx1_ddr3`)

SystemC/TLM model of the FTEL DDR3 controller IP (HAS V1.0, MAS V0.1) for the FX1 virtual
platform. The model is functional and event-timed. It is meant for firmware/driver
development and SoC integration, not for cycle or bandwidth accuracy.

**Status: subtask 5 of 5 (integrated in `fx1_soc`, accepted on the CPU).** The complete CSR map, commits, mutex,
snapshots, interrupts, initialization, boot/runtime training, readiness, refresh, P-channel
power, and the memory data path (1–4 AXI ports, region decode, ordering, exclusive access,
mapping, approximate per-bank timing, SECDED companion ECC with RMW, startup scrub, read
quarantine, fatal containment), host debug/preload and fault injection, and the platform
bridge `GenericAxiBridge` are implemented and covered by 101 regression cases. Every audited
requirement is tested or listed as an agreed limitation
([verification_report.md](docs/verification_report.md)). The IP is the DDR of `fx1_soc`, with
DDR_SYSCTRL glue, a preinitialized boot (default) and a firmware-init boot from BootROM/SRAM;
CPU, DMA, ISP, atomic and FreeRTOS workloads pass on it
([acceptance_report.md](docs/acceptance_report.md)). Field encodings that
HAS/MAS leave undefined are PROVISIONAL (`include/fx1_ddr3/provisional_encodings.h`, DEC-13).
Firmware register header: `include/fx1_ddr3/fx1_ddr3_regs.h` (C99).

## Build and test

Standalone (component only):

```bash
cd components/FX1_Components/ddr3
make test                    # Ninja + /usr/bin/g++, SystemC from /opt/systemc-2.3.4
make sanitize                # ASan/UBSan build; needs libasan and libubsan installed
```

Inside CDC-VP, it builds with the FX1 SoC and its tests carry the `fx1_unit` and `ddr3` labels:

```bash
export PATH=/usr/bin:/bin:$PATH CC=/usr/bin/gcc CXX=/usr/bin/g++
cmake -S . -B build-fx1-ddr3 -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DSYSTEMC_HOME=/opt/systemc-2.3.4 -DCDC_BUILD_FX1_SOC=ON -DFX1_SOC_BUILD_FIRMWARE=ON \
  -DCDC_BUILD_TESTS=ON -DFX1_RISCV_PREFIX=/opt/toolchains/riscv-none-elf/bin/riscv-none-elf-
cmake --build build-fx1-ddr3 --target fx1_all
ctest --test-dir build-fx1-ddr3 -L ddr3 --output-on-failure --no-tests=error
```

Options: `FX1_DDR3_BUILD_TESTS` (default: `BUILD_TESTING` standalone, `CDC_BUILD_TESTS` in
CDC-VP) and `FX1_DDR3_SANITIZE`.

Targets: `cdc::components::fx1_ddr3_core` (pure C++17) and `cdc::components::fx1_ddr3`
(SystemC/TLM). A standalone install exports the `fx1-ddr3` CMake package.

## Minimal use

```cpp
#include "fx1_ddr3/ddr3_controller.h"

fx1::ddr3::BuildConfig cfg = fx1::ddr3::BuildConfig::native_r2();  // or native_r1(), legacy()
cfg.n_ports = 1;
fx1::ddr3::Ddr3Controller ddr("ddr", cfg);        // owns a FunctionalPhy unless one is passed
bus_csr_socket.bind(ddr.csr_socket);              // 64 KiB AXI4-Lite aperture, 16-bit offsets
fx1::ddr3::GenericAxiBridge bridge("bridge", cfg.axi_beat_bytes());  // generic bus -> AXI
bus_ddr_socket.bind(bridge.target);
bridge.initiator.bind(ddr.mem_socket[0]);         // AXI4 transactions with fx1::ddr3::AxiExtension
ddr.irq.bind(irq_signal);
ddr.set_bootstrap_region(0, 0, {0x80000000u, 0x20000000u});
ddr.qos_set_configured(0, true);                  // FE05: no admission before the QoS table
// Firmware then programs TOPO/TIMING/DFI, commits, sets GLOBAL_CTRL.enable and waits for
// mission readiness (initialization, training and, with ECC, the startup scrub).
```

## Documents

| File | Content |
|---|---|
| [docs/programming_guide.md](docs/programming_guide.md) | **Firmware/driver guide**: access rules, boot sequence with register values, runtime operations, interrupts and error recovery |
| [docs/integration_guide.md](docs/integration_guide.md) | **Integrator guide**: build/link, instantiation, binding, sideband, boot modes, preload, host API |
| [docs/fault_catalogue.md](docs/fault_catalogue.md) | Every injectable fault: API, firmware-visible effect, recovery, test |
| [docs/limitations.md](docs/limitations.md) | Agreed limitations (L-01..L-21), provisional encodings, QoS sideband, CPU speed on the VP |
| [docs/acceptance_report.md](docs/acceptance_report.md) | **Subtask 5 acceptance**: both boot modes, CPU tests, workloads, wall time |
| [docs/verification_report.md](docs/verification_report.md) | Coverage, regression content, seeded soak, mutation checks, efficiency, gaps |
| [docs/baseline.md](docs/baseline.md) | Pinned specification hashes, repository base commit, toolchain |
| [docs/decision_log.md](docs/decision_log.md) | Specification and user decisions, HAS/MAS conflict resolutions, open-issue policy |
| [docs/spec_owner_questions.md](docs/spec_owner_questions.md) | Questions for the HAS/MAS owners; Part A blocks dependent features |
| [docs/register_inventory.md](docs/register_inventory.md) | The 210 CSR words from HAS §8, access classes, resets, capability constants |
| [docs/architecture.md](docs/architecture.md) | Abstraction level, code structure, block plan, timing and storage rules |
| [docs/interfaces.md](docs/interfaces.md) | CSR, memory-port, IRQ/status, sideband, PHY, debug and platform contracts |
| [docs/traceability.md](docs/traceability.md) | Requirement → subtask → status matrix (generated) |
| [docs/audit/](docs/audit/) | Per-section HAS/MAS audits: requirements, conflicts, gaps, golden examples |
