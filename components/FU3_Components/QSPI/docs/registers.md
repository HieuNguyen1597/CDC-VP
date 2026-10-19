# Register interface

All registers reset to zero. Offsets are local to AP_QSPIC (`0x44000000` in the
FU3 map). Accesses must be aligned 32-bit words; values are little-endian.

| Offset | Register | Writable mask | Behavior | PDF page |
| --- | --- | --- | --- | --- |
| 0x00 | CR | 0xffff1fff | Enable, abort, polling, thresholds, IRQ enables, prescaler | 2–4 |
| 0x04 | DCR | 0x001f0701 | FSIZE, CSHT, CKMODE | 4 |
| 0x08 | SR | RO | Flags, BUSY, TX/RX byte levels | 5 |
| 0x0c | FCR | 0x0000003f | W1C SR flags 0–5; reads zero | 6 |
| 0x10 | DLR | 0xffffffff | Transfer byte count minus one; all ones means flash-end length | 6–7 |
| 0x14 | CCR | 0x9f7fffff | Serial phases, DDR, SIOO and functional mode; may trigger | 7–8 |
| 0x18 | AR | 0xffffffff | Address; may trigger; ignored in memory-mapped mode | 9 |
| 0x1c | ABR | 0xffffffff | Alternate bytes | 9 |
| 0x20 | DR | 0xffffffff | TX enqueue/RX dequeue, polling last status, mapped read zero | 10 |
| 0x24 | PSMKR | 0xffffffff | Compared polling bits; 1 means considered | 10 |
| 0x28 | PSMAR | 0xffffffff | Polling expected bits | 11 |
| 0x2c | PIR | 0x0000ffff | Poll gap in core clock cycles, respecting CSHT | 11 |
| 0x30 | PTR | 0xffffffff | Poll timeout in core clock cycles | 12 |
| 0x34 | DMACR | 0x0000000f | TX/RX DMA enable; TX overrun/RX underrun IRQ enable | 12–13 |
| 0x38 | DMASR | RO | TXOVRF bit 2, RXUDRF bit 3 | 13 |
| 0x3c | DMAFCR | 0x0000000c | W1C DMA error flags; reads zero | 14 |

SR bits 0–6 are TEF, TCF, TXFTF, RXFTF, SMF, STOF, BUSY. TXFLEVEL is bits
13:8 and RXFLEVEL is bits 21:16. CR IRQ enables are bits 16–21 in the same
order as SR flags 0–5. IRQ is the OR of these enabled flags and enabled
DMASR error flags, independent of DMA enable bits.

CR bit 2 is PTCEN, bit 22 APMS, bit 23 PMM. TXFIFOTH is bits 12:8 and
RXFIFOTH bits 7:3. See [model choices](../README.md#model-choices) for the
inconsistent source FIFO units.
PRESCALER is bits 31:24; its divisor must be even for SDR and divisible by
four for DDR. The reset divisor is therefore not legal for a started transfer.

CCR FMODE bits 27:26 select write=0, read=1, poll=2, mapped=3.
IMODE/ADMODE/ABMODE/DMODE encode absent=0, single=1, dual=2, quad=3.
ADSIZE/ABSIZE encode one through four bytes. SIOO suppresses instructions
after the first command following a CCR write; rewriting CCR rearms it.

| Mode | Trigger |
| --- | --- |
| Indirect write, no data, no address | CCR write |
| Indirect write, no data, address present | AR write |
| Indirect write, data present | First DR write |
| Indirect read or polling, no address | CCR write |
| Indirect read or polling, address present | AR write |
| Memory-mapped | AHB/XIP read |

When BUSY, configuration writes are ignored. CR EN/ABORT/IRQ enables and
DMACR remain writable. FCR/DMAFCR and DR retain their normal side effects.
ABORT self-clears, closes an open frame, flushes FIFOs and sets TCF.
