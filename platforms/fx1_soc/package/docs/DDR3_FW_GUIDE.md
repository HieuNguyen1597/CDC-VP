# VP_FX1_V3.0 — DDR3 controller: guide for firmware and driver developers

Audience: firmware engineers who write or test a DDR3 driver, a boot loader or error handling on the FX1 VP. To run existing firmware, nothing in this folder is needed: the VP initializes the DDR3 before the harts start.

## 1. What is in this delivery

| Path (from the SDK root) | Content |
|---|---|
| `vp/doc/VP_FX1_V3.0/ddr3/README.md` | This guide |
| `vp/doc/VP_FX1_V3.0/ddr3/programming_guide.md` | **The register-level guide**: access rules, initialization sequence with the reference values, readiness, region map, runtime operations (quiesce, training, power, refresh), interrupts, error handling and recovery, bring-up checklist |
| `vp/doc/VP_FX1_V3.0/ddr3/fault_catalogue.md` | Every fault the VP can inject, what firmware observes, how to recover, and the `--ddr3-fault` option (§7) |
| `vp/doc/VP_FX1_V3.0/ddr3/limitations.md` | What the model does not do or does differently from silicon |
| `vp/doc/VP_FX1_V3.0/ddr3/spec_owner_questions.md` | Field encodings that the specification leaves open (Part A): the VP implements a proposal for each |
| `sw/bootloader/sources/VP_FX1_V3.0/include/fx1_ddr3/fx1_ddr3_regs.h` | Register map (C99) |
| `sw/bootloader/sources/VP_FX1_V3.0/include/fx1_ddr3/provisional_encodings.h` | The provisional codes; use the macros, never the numbers |
| `sw/bootloader/sources/VP_FX1_V3.0/include/fx1/fx1_ddr_sysctrl.h` | DDR_SYSCTRL (VP only, §4) |
| `sw/bootloader/sources/VP_FX1_V3.0/bsp/crt0_rom.S`, `bsp/fx1_rom.ld.S` | Start-up and linker script for images that run from BootROM with data and stacks in SRAM |
| `sw/bootloader/sources/VP_FX1_V3.0/ddr3/` | Reference bring-up code and the examples (§6) |

The programming guide was written in the VP source tree. Where it names a path there, use this table:

| Path in the programming guide | Path in this SDK |
|---|---|
| `fw/fx1_soc/ddr3/ddr3_fw.c`, `ddr3_fw.h`, `ddr3_ref_1600.h` | `sw/bootloader/sources/VP_FX1_V3.0/ddr3/` |
| `fw/fx1_soc/tests/ddr3/*.c`, `fw/fx1_soc/boot/boot_loader.c` | `sw/bootloader/sources/VP_FX1_V3.0/ddr3/examples/` |
| `fw/fx1_soc/common/crt0_rom.S`, `fx1_rom.ld.S` | `sw/bootloader/sources/VP_FX1_V3.0/bsp/` |
| `test_fx1_ddr3_subsystem` (a VP-side test) | Not shipped: the DDR3-1600 values in `ddr3_ref_1600.h` were checked against the model there |

## 2. The controller on the VP

| Item | Value |
|---|---|
| Build | NATIVE_R2 (2 tCK per controller cycle), DDR3-1600, 2 ranks of 4 Gb x8 on a x32 bus = 4 GiB physical, ECC on |
| CPU aperture | `0x8000_0000`, 512 MiB, port 0 (decoded by the bootstrap map until firmware commits a region map) |
| CSR | `0x1200_0000`, 64 KiB, 32-bit aligned accesses only |
| DDR_SYSCTRL | `0x1201_0000` (VP only) |
| Interrupt | PLIC source 8 (`FX1_IRQ_DDR`), a level; IRQ_MASK resets to 0 |
| Boot SRAM | `0x0100_0000`, 1 MiB |

A CSR access that the controller rejects (SLVERR or DECERR) reaches the CPU as a load or store access fault (mcause 5 or 7). `ddr3/ddr3_probe.S` shows how to make such an access without trapping out of the firmware.

## 3. Boot modes

| `fx1_soc` option | Who initializes the DDR3 | Image layout |
|---|---|---|
| `--ddr-boot preinit` (default) | The VP. It runs the same register sequence as §4 of the programming guide, waits for readiness including the ECC scrub (672 ms of simulated time, a few ms of host time) and loads the ELF and the `--load` files. Only then do the harts start. `--timeout-ms` and the console replay delay count from that start | Ordinary DDR image (`bsp/crt0.S`, `bsp/fx1.ld.S`) |
| `--ddr-boot firmware` | The firmware, running from BootROM with data and stacks in SRAM. The harts start at time 0 | BootROM image (`bsp/crt0_rom.S`, `bsp/fx1_rom.ld.S`). An ELF with DDR segments, or `--load`, is refused (exit status 3): the scrub would erase it |

In firmware boot the time budget includes the initialization: allow at least 1 s of `--timeout-ms` for one bring-up (the scrub of 4 GiB takes about 670 ms), and give the readiness wait in firmware a timeout above that (the examples use 2 s).

## 4. DDR_SYSCTRL

The controller has no CSR for its readiness pins, its power-state channel, its QoS sideband or its resets. In the VP, DDR_SYSCTRL provides them. It is **not** FX1 silicon: keep its use behind a platform layer.

| Offset | Register | Use |
|---|---|---|
| 0x004 | STATUS | [0] mission ready, [1] initialization complete, [2] global idle, [3] front end drained, [4] interrupt line, [8] the VP preinitialized the DDR |
| 0x010 / 0x014 | PWR_REQ / PWR_STATUS | Request a power state (`FX1_DDRSC_PWR_REQ_VAL(pstate)`), then poll BUSY; ACCEPTED or DENIED gives the outcome |
| 0x018 | PWR_CTRL | AUTO_WAKE (reset 1): a DDR access in a low-power state wakes the controller by itself. A denied wake is retried with a back-off and shown in PWR_STATUS[3] |
| 0x020 / 0x024 | RST_CTRL / RST_STATUS | Cold reset, or coordinated core reset (request, reset-safe, pulse) |
| 0x040 + 4p / 0x050 | QOS_LUT(p) / QOS_CFG | QoS table of port p and "configured" bits. A port admits nothing until configured; a cold reset clears it |

The full bit definitions are in `fx1_ddr_sysctrl.h`.

## 5. Bringing the DDR3 up from firmware

The sequence is §4 of the programming guide; `ddr3/ddr3_fw.c` implements it step by step and is the reference to compare a driver with:

1. Check `IP_ID`, `ABI_VERSION` and `CAP_FEATURE` (`ddr3_identify`).
2. Configure the port-0 QoS table through DDR_SYSCTRL (`ddr3_qos_default`). This is required again after a cold reset.
3. Write the timing and DFI shadows, every INIT_RW word and TOPO_SHADOW (`ddr3_program`, values from `ddr3_ref_1600.h`). INIT_RW words lock once initialization starts.
4. Commit the topology, then the timing, then DFI (`ddr3_commit_all`). A commit that fails answers SLVERR; read the commit status blocker fields.
5. Set `GLOBAL_CTRL.enable`.
6. Wait for DDR_SYSCTRL `STATUS.mission_ready` with a timeout, and also check `INIT_STATUS` (after a snapshot) and `FATAL_STATUS` (`ddr3_wait_ready`). Report a failure over the UART without touching DDR.
7. Only now place code or data in DDR. Before readiness the scrub zeroes it.

While hart 0 does this, keep the other hart out of DDR: `crt0_rom.S` parks it in WFI until hart 0 releases it.

## 6. Examples

Build: `make -C sw/bootloader/sources/VP_FX1_V3.0 ddr3` (outputs in `sw/bootloader/build/VP_FX1_V3.0/ddr3/`). Run them all: `sw/bootloader/test/VP_FX1_V3.0/run_vp.sh ddr3`.

| Example | Shows | Run alone |
|---|---|---|
| `examples/fw_init.c` | Identity and reset state; a rejected TIMING_COMMIT (SLVERR, blocker 0x04, IRQ_RAW[4]); initialization and readiness; word/half/byte accesses; a region map whose decode miss gives a load access fault and a PLIC interrupt; self-refresh with automatic wake; both harts with an AMO counter in DDR | `fx1_soc --fw ddr3_fw_init.elf --ddr-boot firmware --timeout-ms 3000` |
| `examples/fault_recovery.c` | Correctable and uncorrectable ECC errors, a read-return timeout and quarantine, recovery by cold reset and re-initialization; a fatal event recovered by the other hart with a core reset, then a cold reset | add `--ddr3-fault 1000000:ce:0x80100000 --ddr3-fault 1010000:ue:0x80100040 --ddr3-fault 1020000:read-timeout --ddr3-fault 2000000:fatal:dfi --timeout-ms 4000` (the schedule is compiled in as `FAULT_*_MS`) |
| `examples/power_recovery.c` | A failed self-refresh exit: the automatic wake is denied; the other hart sees it and recovers with a cold reset; re-initialization | `--ddr-boot firmware --timeout-ms 3000` |
| `examples/boot_loader.c` | A BootROM loader: DDR3 bring-up, copy of an ELF embedded in BootROM to DDR, start of both harts. The Makefile embeds `freertos_cli.elf` (`boot_freertos.elf`) | `--ddr-boot firmware --timeout-ms 5000`, with the console replay of `run_vp.sh ddr3` |

These are test and reference code, not a driver library.

## 7. Faults, interrupts and recovery

- Faults are injected only from the host: `--ddr3-fault T_US:KIND[:ARG]` (`fx1_soc --help`), T_US counted from CPU start. [fault_catalogue.md](fault_catalogue.md) lists each kind and what firmware observes.
- Interrupt sources and their handling: programming guide §7. Clear a source at the controller (IRQ_CLEAR) before completing it at the PLIC.
- Recovery needs DDR_SYSCTRL resets: a quarantine or a fatal event clears only on a cold reset followed by a full initialization. With ECC the new scrub zeroes DDR, so the recovery code must run from BootROM/SRAM. An access held by a fatal event can only be ended by a reset from another agent (another hart).
- LR/SC and AMOs on DDR: a cold or core reset, a reinitialization or a region commit voids every reservation (the next SC fails). Region aliases name the same storage for reservations, AMOs and DMA writes.

## 8. Limits to keep in mind

- **Provisional encodings.** Some field codes are not defined by the specification yet (Part A of [spec_owner_questions.md](spec_owner_questions.md)). The VP implements a proposal for each, in `provisional_encodings.h`. These values will change: use the macros.
- **Timing is approximate, and CPU timing is pessimistic.** The harts fetch every instruction from DDR (no cache in the VP). Do not calibrate CPU-versus-DMA races or delays on the VP.
- **The VP-only blocks.** DDR_SYSCTRL and the `--ddr3-fault` option do not exist on silicon.

The full list is in [limitations.md](limitations.md).
