# VP_FX1_V3.0 memory and interrupt map

The authoritative source is
`sw/bootloader/sources/VP_FX1_V3.0/include/fx1/fx1_memory_map.h`. This page is
a reading aid.

**All values are VP placeholders, pending the FX1 HAS.** Firmware must use
the header macros, never literal addresses, so that a map update is only a
rebuild. `BUILD_MANIFEST.json` lists the values this binary was built with.

## Address map

| Region | Base | Size | Macro | Notes |
|---|---|---|---|---|
| BootROM | `0x0000_0000` | 2 MiB | `FX1_BOOTROM_BASE` | read-only; a store faults. Not used at bring-up (the ELF is loaded into DDR) |
| CLINT | `0x0200_0000` | 64 KiB | `FX1_CLINT_BASE` | SiFive layout: `msip` +0x0000, `mtimecmp` +0x4000, `mtime` +0xBFF8; `mtime` at 20 MHz (`FX1_MTIME_HZ`) |
| PLIC | `0x0C00_0000` | 16 MiB | `FX1_PLIC_BASE` | SiFive layout; context 0 = hart 0 M-mode, context 1 = hart 1 M-mode |
| UART | `0x1000_0000` | 64 KiB slot | `FX1_UART_BASE` | PL011-style: DR, FR, IBRD/FBRD, LCR_H, CR, IFLS, IMSC/RIS/MIS/ICR; 16-entry FIFOs |
| SYS_DMA CSR | `0x1001_0000` | 64 KiB slot | `FX1_SYS_DMA_CSR_BASE` | register map in `include/dma/fx1_dma_regs.h` |
| sim-control | `0x1002_0000` | 64 KiB slot | `FX1_SIM_CTRL_BASE` | **VP only, not on silicon**: PASS/FAIL verdict ends the simulation (`fx1_pass`, `fx1_fail`) |
| ISP CSR | `0x1100_0000` | 64 KiB | `FX1_ISP_CSR_BASE` | register map in `include/fx1_isp/fx1_isp_csr.h` (generated from the ISP CSR specification) |
| NPU, H.264/H.265, Ethernet, MIPI CSR | `0x1101_0000` … `0x1104_0000` | 64 KiB each | `FX1_*_CSR_BASE` | reserved: not modelled, an access faults |
| DDR | `0x8000_0000` | 512 MiB | `FX1_DDR_BASE` | sparse model; the DDR controller's CSRs and training are not modelled |

The CPU sees an access fault (load mcause 5, store mcause 7) on:

- an unmapped address, including the reserved devices;
- a byte or halfword access to UART or SYS_DMA. Hart 1 checks both at boot.

Treat every register block as word-only. Use naturally aligned 32-bit
accesses (`fx1_read32` / `fx1_write32`); other sizes are not part of the
contract.

## Interrupts

| PLIC source | Macro | Device |
|---|---|---|
| 1 | `FX1_IRQ_UART` | UART (RX, RX timeout, TX, errors; enabled in `IMSC`) |
| 2 | `FX1_IRQ_SYS_DMA` | SYS_DMA channel completion/error |
| 3 | `FX1_IRQ_ISP` | ISP (frame done, DMA events, statistics) |
| 4–7 | `FX1_IRQ_NPU`, `_H264_H265`, `_ETH`, `_MIPI` | reserved |

Interrupt delivery:

- **External interrupts:** the PLIC raises `mip.MEIP` on each hart (claim and
  complete per context).
- **Timer:** CLINT `mtimecmp[hart]` raises `mip.MTIP`.
- **Software:** CLINT `msip[hart]` raises `mip.MSIP`; hart 0 uses it to release
  hart 1 at boot.

## DDR use by the delivered firmware

| Range | Macro | Use |
|---|---|---|
| `0x8000_0000` + 4 MiB | `FX1_FW_IMAGE_BASE` | ELF code and data (both harts) |
| `0x8040_0000` + 64 KiB per hart | `FX1_FW_STACK_BASE` | hart *n* stack at base + *n* × 64 KiB |
| `0x8050_0000` + 11 MiB | `FX1_FW_SHARED_BASE` | shared data. For the ISP job: job at +0x0000, result block at +0x0100, profile at +0x1000 (`demo/isp_demo_job.h`) |
| `0x8100_0000` | `FX1_FW_RAW_BUF_BASE` | ISP RAW input |
| `0x8800_0000` | `FX1_FW_YUV_BUF_BASE` | ISP NV12 output: Y, then UV at the next 1 MiB boundary |
| `0x9000_0000` | `FX1_FW_TEST_AREA_BASE` | free test area (safe for `mw` experiments) |

## Harts

- **Configuration:** 2 harts, `rv32imafc_zicsr_zifencei`, M-mode only. No MMU,
  no caches.
- **Boot:** both harts start at the ELF entry. Hart 1 parks in WFI until hart
  0 sends an MSIP.
- **Atomics:** LR/SC and AMOs go through the bus exclusive monitor. A
  reservation granule is 64 bytes, and DMA writes also clear reservations.
