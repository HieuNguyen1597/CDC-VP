# DDR3 reference code and firmware-init examples

Reference bring-up code for the DDR3 controller of the FX1 VP, and examples
that start from BootROM (data and stacks in SRAM) and initialize the DDR3
themselves. Test and reference code, not a driver library.

| File | Content |
|---|---|
| `ddr3_fw.h`, `ddr3_fw.c` | Programming-guide §4 sequence, readiness wait, DDR_SYSCTRL power/reset requests |
| `ddr3_ref_1600.h` | DDR3-1600 reference image (timing, DFI, INIT waits, topology) |
| `ddr3_probe.S` | Register access that returns the access fault instead of trapping |
| `examples/` | `fw_init.c`, `fault_recovery.c`, `power_recovery.c`, `boot_loader.c` |

```bash
make -C sw/bootloader/sources/VP_FX1_V3.0 ddr3        # -> sw/bootloader/build/VP_FX1_V3.0/ddr3/*.elf
sw/bootloader/test/VP_FX1_V3.0/run_vp.sh ddr3         # runs all of them with --ddr-boot firmware
```

Guide: `vp/doc/VP_FX1_V3.0/ddr3/README.md`; register-level details in
`vp/doc/VP_FX1_V3.0/ddr3/programming_guide.md`.
