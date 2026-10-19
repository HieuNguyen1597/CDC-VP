# FU3 QSPI SystemC/TLM VP

Functional SystemC/TLM model for QSPI, AP_QSPIC, A2H_QSPI and XIP, based on
`QSPI_Register_Description.pdf` and `QSPI_Wrapper_Interface.pdf`.
Supports the 16-register map, indirect transfers, polling, XIP, FIFOs, IRQ and
DMA. Includes a generic NOR flash backend for firmware/image testing.

## Build and test

From the repository root in Linux/WSL with SystemC 2.3.4 and C++17:

```sh
cmake -S components/FU3_Components/QSPI -B build/fu3-qspi \
  -DSYSTEMC_HOME=/opt/systemc-2.3.4 -DCMAKE_BUILD_TYPE=Debug \
  -DFU3_QSPI_BUILD_TESTS=ON
cmake --build build/fu3-qspi -j4
ctest --test-dir build/fu3-qspi --output-on-failure
```

The default suite has 44 tests. `FU3_QSPI_BUS_TESTS=OFF` omits the real FU3 bus
integration case. The repository build enables this component through
`CDC_BUILD_FU3_SOC=ON`; link the CMake target `fu3::qspi`.
See [testing](docs/testing.md) for P0/P1 selection, source clauses and UBSan.

## Components

Headers are under `include/fu3/qspi/`.

| Class | Header | Responsibility |
| --- | --- | --- |
| QspiRegisterModel | qspi_register_model.h | Register storage, reset, masks, access policy, W1C |
| QspiController | qspi_controller.h | FSM, triggers, BUSY/ABORT/TCF/TEF, AP/XIP sockets |
| QspiTxRxFifo | qspi_tx_rx_fifo.h | TX/RX storage, levels and threshold flags |
| QspiFlashModel | qspi_flash_model.h | NOR storage, commands, program/erase, image I/O |
| QspiAhbAdapter | qspi_ahb_adapter.h | XIP aperture and functional forwarding |
| QspiDmaInterface | qspi_dma_interface.h | Request/clear, burst eligibility, rearm |
| QspiInterrupt | qspi_interrupt.h | Enabled status/error flag aggregation |
| QspiTimingModel | qspi_timing_model.h | Divider, serial phase, CSHT/PIR and timeout timing |

Controller, flash and adapter are SystemC modules. The other classes are
components driven by the controller processes; their short implementations
live in headers. `Qspi` and `A2hQspi` are compatible aliases. The older
`components/qspi_tlm` uses a different register map/protocol.

## Integration

```cpp
#include <fu3/qspi/qspi_controller.h>
#include <fu3/qspi/qspi_ahb_adapter.h>
#include <fu3/qspi/qspi_flash_model.h>

fu3::qspi::QspiController qspi{"qspi"};
fu3::qspi::QspiAhbAdapter a2h{"a2h"};
fu3::qspi::QspiFlashModel nor{"nor"};
fabric.target("AP_QSPIC").bind(qspi.ap_qspic);
fabric.target("QSPI_XIP").bind(a2h.socket);
a2h.ahb.bind(qspi.xip);
qspi.flash.bind(nor.socket);
qspi.clk(qspi_clk); qspi.pclk(apb_clk); qspi.hclk(ahb_clk);
qspi.reset_n(core_reset_n); qspi.preset_n(apb_reset_n);
qspi.hreset_n(ahb_reset_n); nor.reset_n(flash_reset_n);
qspi.irq(qspi_irq);
qspi.dma_rx_req(rx_request); qspi.dma_rx_clr(rx_clear);
qspi.dma_tx_req(tx_request); qspi.dma_tx_clr(tx_clear);
nor.load_image("boot.bin"); // Raw binary; load before transactions start.
```

Bind every clock, reset, IRQ and DMA signal. Disable or bind other enabled
fabric targets as described in the FU3 bus README. The fabric supplies local
addresses: AP_QSPIC is mapped at `0x44000000` (64 KiB, implemented offsets
`0x00..0x3c`), XIP at `0x30000000` (128 KiB). These addresses come from the
existing FU3 bus map. FSIZE additionally limits accesses.

Call `b_transport` from an `SC_THREAD`. Bus accesses are aligned, little-endian,
full-lane 32-bit words. The fabric decomposes CPU/DMA bursts into singles.
Partial/narrow accesses fail; XIP currently accepts reads only. AP debug reads
are non-destructive; debug writes and DMI are unavailable.

## Register use

See [registers](docs/registers.md) for offsets, fields and trigger rules.
A minimal 128-KiB XIP setup is:

```cpp
write32(0x44000004, 16u << 16); // FSIZE = 16.
write32(0x44000000, 0x01000001); // EN; SCLK = core clock / 2.
write32(0x44000014, 0x0d002503); // Mapped READ 03, 1-1-1, 24-bit address.
auto first_word = read32(0x30000000);
```

For indirect reads, program DLR to byte count minus one, CCR to FMODE=1,
then AR when ADMODE is nonzero. Drain DR while RXFLEVEL is nonzero. TCF marks
serial completion; BUSY clears once RX is empty. For indirect writes with data,
the first DR write starts transmission. Clear sticky flags through FCR/DMAFCR
before reusing them as completion tests.

## Flash backend

`QspiFlashModel` is a generic SDR NOR profile, not a complete named vendor chip.
Storage starts at `0xff`; capacity is a power of two from 4 KiB to 16 MiB,
default 128 KiB. `FlashConfig` sets size, operation durations and the synthetic
ID (default `7f 51 11`). Command conventions use the
[W25Q64JV Revision C datasheet, sections 7-8](https://media.digikey.com/pdf/Data%20Sheets/Winbond%20PDFs/W25Q64JV_Rev_C.pdf);
the two FU3 PDFs do not specify a flash device.

| Opcode (hex) | Operation / requirements |
| --- | --- |
| 06 / 04 | Write enable / disable |
| 05 / 35 | Read repeated status: WIP/WEL / QE |
| 31 / 9f | Write QE (one byte, only bit 1, WEL required) / read repeating 3-byte ID |
| 03 / 0b | 1-1-1 read: no dummy / eight dummy clocks |
| 3b / 6b | 1-1-2 / 1-1-4 read, eight dummy clocks; quad requires QE |
| bb / eb | 1-2-2 / 1-4-4 read, one alternate byte; zero / four dummy clocks; quad requires QE |
| 02 / 32 | Single / quad page program; WEL required, quad also requires QE |
| 20 / d8 | Aligned containing 4-KiB sector / 64-KiB block erase; WEL required |
| c7 / 60 | Chip erase; WEL required |
| 66 then 99 | Consecutive enable-reset / reset commands |

Address phases are three bytes. A program frame accepts 1..256 bytes and wraps
within its 256-byte page; commit uses AND, so program only clears bits. Program,
erase and QE writes consume WEL and set WIP until an atomic commit. Controller
TCF can precede flash completion: poll WIP before reading/programming again.
Only status reads and reset are accepted while WIP is set. Invalid commands or
phase combinations return TLM errors. Defaults are VP timings: program 50 us,
sector erase 200 us, block erase 500 us, chip erase 2 ms, QE write 5 us.

EB alternate byte high nibble `a` enters continuous read for SIOO; another
nibble exits it. An explicit instruction replaces the retained command.
Flash reset clears WEL/QE/continuous state and discards unfinished operations,
while committed bytes survive. Cancel before frame close discards staged data;
it cannot undo a frame already accepted. Load/save require idle flash:

```cpp
nor.load(bytes, offset);
nor.load_image("boot.bin", offset);
auto snapshot = nor.read_image(0, 16); // Last committed bytes, also while busy.
nor.save_image("flash.bin");
```

A custom flash target can consume `FlashTransaction`: at most four bytes per
callback, frame ID, total length, offset, begin/end/cancel, opcode, numeric
address/alternate and serial phase metadata. Targets must annotate latency,
return a response and never call `wait` or retain payload pointers. The
controller waits for annotated latency before exposing data/completion.
SFDP, protection/OTP, suspend/resume, QPI, four-byte flash addresses, DDR flash
commands, ELF loading and automatic persistence are not included.

## Model choices

Where source wording is incomplete, these are model decisions rather than
confirmed RTL behavior. R = register PDF; W = wrapper PDF. PDF originals were
supplied from `D:/Intern_doc/Intern/VP/IP/QSPI/`.

| Topic | Behavior and source boundary |
| --- | --- |
| FIFO units | R p.3 describes words, p.5 bytes. Storage/levels count bytes (default 32); `Config::flag_threshold_unit` selects byte flags (default) or word flags. DMA thresholds always count words; encoded thresholds >=8 cannot fit 32-byte storage. |
| Partial DR word | TX uses remaining DLR bytes; RX zero-fills high lanes. A final partial RX word counts as one readable DMA word. |
| DMA | R 1.14 p.13: TX requires free threshold words and bytes not all accepted into DR, without a remaining-length >= burst gate. Prefill/rearm sequencing is a VP convention. Clear suppresses a request for its sampled cycle; held clear keeps it low. |
| Independent resets | Core reset clears state. PRESET cancels AP callers, HRESET cancels XIP; isolated bus resets preserve core configuration. W specifies domains, not isolated-reset register ownership. |
| Mapped writes / invalid accesses | W exposes HWRITE but does not define mapped-write behavior; this model rejects writes. Unaligned/unmapped accesses fail, RO writes are ignored, reserved bits masked; empty frames and addresses exceeding ADSIZE are rejected as model policies. |
| Polling | CR bit 2 is PTCEN (also called TCEN in later text). PTR is a polling timeout. Empty AND matches, empty OR does not; APMS match wins over simultaneous timeout. The latter rules need source clarification. |
| Disable / abort | EN=0 cancels and flushes while preserving sticky flags/configuration (model choice). ABORT self-clears, flushes and sets TCF per R p.4. |

Core progress is clocked; AP admission uses PCLK, XIP admission HCLK. Incoming
TLM delay is consumed; clock gating freezes its domain. Bus timeouts count bus
edges, default 5000; resets also invalidate queued callers. Configuration is
frozen during BUSY except EN/ABORT, IRQ enables, DMACR and normal flag/data
accesses. Backend failures set indirect/polling TEF or return an XIP error.

Timing represents phases and minimum CSHT/PIR with possible scheduling gaps.
This model does not reproduce bus/pad waveforms, RTL CDC, asynchronous/command
FIFO occupancy, multi-flash selection or CPU instruction execution. The tests
check this documented functional scope, not full RTL equivalence.
