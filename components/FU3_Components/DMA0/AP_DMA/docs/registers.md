# Register reference

Use aligned 32-bit little-endian accesses. Channel `n` occupies offset
`0x100 * n`, for `n=0..7`. Global offsets are relative to the DMA base and already
include `0x1000`. The minimum MMIO aperture is `0x1100` bytes. Offsets
`0x0800..0x0fff` and unlisted locations are unmapped.

RW means read/write, RO means read-only, and WO means write-only. Reserved
configuration bits are masked on writes. Configuration writes at channel offsets
`0x00..0x24` are rejected while that channel is active.

## Channel registers

Add the channel offset to each entry:

| Offset | Register | Access | Reset | Usage |
|---|---|---|---|---|
| `0x00` | `CH_CMD_READ_ADDR` | RW | `0` | Physical source address |
| `0x04` | `CH_CMD_WRITE_ADDR` | RW | `0` | Physical destination address |
| `0x08` | `CH_CMD_TRANSFER_SIZE` | RW | `0` | Byte count, bits 9:0; program 1..1023 |
| `0x0c` | `CH_CMD_CONTROL` | RW | `0` | Completion interrupt, last-command flag, next descriptor address |
| `0x10` | `CH_READ_CONFIG` | RW | `0x84010000` | Read burst, arbitration, pending limit, and address increment |
| `0x14` | `CH_WRITE_CONFIG` | RW | `0x84010000` | Write burst, arbitration, pending limit, and address increment |
| `0x18` | `CH_MODE_CONFIG` | RW | `0` | Payload endian conversion |
| `0x1c` | `CH_SCHEDULE_CONFIG` | RW | `0` | Reads zero; writes have no effect |
| `0x20` | `CH_PERIPHERAL_CONFIG` | RW | `0` | Endpoint IDs and recovery delays |
| `0x24` | `CH_AXI_ATTR_REG` | RW | `0` | Per-direction CACHE/PROT overrides |
| `0x2c` | `CH_RESTRICTION_STATUS` | RO | `0` | Effective alignment, pending, and direction flags |
| `0x30` | `CH_READ_OFFSET_STATUS` | RO | `0` | Always zero |
| `0x34` | `CH_WRITE_OFFSET_STATUS` | RO | `0` | Always zero |
| `0x38` | `CH_FIFO_STATUS` | RO | `0x20` | Free capacity and bytes available for writes |
| `0x3c` | `CH_OUTSTANDING_STATUS` | RO | `0` | Pending logical read/write contexts |
| `0x40` | `CH_ENABLE` | RW | `1` | Bit 0: allow new grants |
| `0x44` | `CH_START` | WO | `0` | Write bit 0 to start a configured channel |
| `0x48` | `CH_ACTIVE_STATUS` | RO | `0` | Bit 0: read active; bit 1: write active |
| `0x50` | `CH_TRANSFER_COUNT` | RO | `0` | Command and pending completion-event counts |
| `0xa0` | `CH_INTERRUPT_RAW_STATUS` | RW | `0` | Raw interrupt bits; writes set selected bits |
| `0xa4` | `CH_INTERRUPT_CLEAR` | WO | `0` | Write-one-clear interrupt acknowledgement |
| `0xa8` | `CH_INTERRUPT_ENABLE` | RW | `0x1fff` | Interrupt delivery mask |
| `0xac` | `CH_INTERRUPT_STATUS` | RO | `0` | Raw status AND enable mask |

### Command control

| Bits | Field | Usage |
|---|---|---|
| 0 | `SET_INT` | Queue a completion event when the command succeeds |
| 1 | `LAST` | Stop after this command; when zero, fetch the next descriptor |
| 31:2 | `NEXT_ADDRESS` | Four-byte-aligned descriptor address |

Use control value `3` for one command with a completion interrupt. Initial starts
with zero length are rejected. A malformed fetched descriptor faults the channel.
Chaining requires `EngineConfig::command_chain=true`.

### Read and write configuration

Both configuration registers use the same fields and writable mask `0xcf3f007f`:

| Bits | Field | Usage |
|---|---|---|
| 6:0 | `BURST_MAX_BYTES` | Maximum bytes per service; program 1..127 before starting |
| 21:16 | `TOKENS` | Consecutive grants in weighted round-robin arbitration; zero acts as one |
| 27:24 | `OUTSTANDING_LIMIT` | Effective memory limit clamped to 1..4 when enabled |
| 30 | `OUTSTANDING_ENABLE` | Permit multiple pending memory contexts |
| 31 | `INCREMENT` | Increment address after each service; zero selects FIXED |

Outstanding enable zero limits the direction to one context. A peripheral
endpoint always has one credit regardless of these fields. The whole core allows
at most four read and four write contexts. Reset leaves `BURST_MAX_BYTES=0`, so
both configuration registers must be programmed before starting.

### Endian configuration

`CH_MODE_CONFIG[29:28]` selects no conversion (0), reverse each two-byte group
(1), or reverse each four-byte group (2). Value 3 is rejected. Transfer length
must be divisible by the selected group size.

### Peripheral configuration

The writable mask is `0x071f071f`:

| Bits | Field | Usage |
|---|---|---|
| 4:0 | `READ_ID` | 0 for memory; 1..31 for a peripheral read endpoint |
| 10:8 | `READ_DELAY` | 0..7 recovery cycles after a successful read service |
| 20:16 | `WRITE_ID` | 0 for memory; 1..31 for a peripheral write endpoint |
| 26:24 | `WRITE_DELAY` | 0..7 recovery cycles after a successful write service |

Do not set both endpoint IDs to nonzero. Peripheral transfers require a positive
constructor `peripheral_cycle`. Delay applies to the serviced ID across all
channels in that direction. The next service must also wait for the one-cycle
clear pulse to finish.

The DMA0 endpoint assignments are:

| ID | Peripheral |
|---|---|
| 1 | SPI0 |
| 2 | USART0 |
| 3 | USART1 |
| 4 | QSPI |

Connect these IDs to the platform's request signals and mapped data registers.
The model accepts IDs 1..31; the platform supplies the actual address map.

### AXI attribute overrides

The writable mask is `0x0000bfbf`:

| Bits | Field | Usage |
|---|---|---|
| 3:0 | `READ_CACHE` | Read CACHE override |
| 5:4 | `READ_PROT` | Two-bit read PROT override |
| 7 | `READ_OVERRIDE` | Enable read overrides |
| 11:8 | `WRITE_CACHE` | Write CACHE override |
| 13:12 | `WRITE_PROT` | Two-bit write PROT override |
| 15 | `WRITE_OVERRIDE` | Enable write overrides |

With an override disabled, the direction uses its memory or peripheral defaults
from `EngineConfig`. Integration PROT defaults accept three bits, while this
register's override fields accept two. Descriptor fetches always use memory
defaults.

### Restriction and transfer status

`CH_RESTRICTION_STATUS` is refreshed when relevant configuration is written or a
command is loaded:

| Bit | Meaning |
|---|---|
| 0 | Source address is aligned to 32 bytes |
| 1 | Destination address is aligned to 32 bytes |
| 2 | Both addresses are aligned to 32 bytes |
| 5 | Effective read pending limit is greater than one |
| 6 | Effective write pending limit is greater than one |
| 8 | Both endpoints select memory |

`CH_FIFO_STATUS[5:0]` reports free bytes after accounting for buffered data,
partial endian groups, and pending read/write reservations. Bits 21:16 report
buffered bytes available for assignment to new writes.

`CH_OUTSTANDING_STATUS[3:0]` counts pending read contexts and bits 11:8 count
pending write contexts. These remain allocated until responses drain, including
after a timeout. `CH_ACTIVE_STATUS` can remain nonzero while a channel is paused
or draining accepted work.

`CH_TRANSFER_COUNT[11:0]` counts successful commands since the latest start and
wraps at 4096. Bits 19:16 count pending completion events, saturating at 15.
Writing clear bit 0 acknowledges one event. Starting a channel resets both
counters and clears raw completion bit 0.

## Global registers

All offsets below are relative to the DMA base:

| Offset | Register | Access | Reset | Usage |
|---|---|---|---|---|
| `0x1000` | `CORE_STATUS` | RO | `0` | Bits 7:0 identify channels with enabled pending interrupts |
| `0x1030` | `CORE_JOINT_CONFIG` | RW | `0` | Reads zero; writes have no effect |
| `0x1038` | `CORE_PRIORITY_CONFIG` | RW | `0` | Stores bits 15:0; does not change weighted arbitration |
| `0x1040` | `CORE_CLOCK_DIVIDER` | RW | `0` | Reads zero; writes have no effect |
| `0x1048` | `CORE_CHANNEL_START` | WO | `0` | Bits 7:0 start selected channels; all selected channels must be startable |
| `0x1050` | `PERIPHERAL_RX_REQUEST` | RW | `0` | Software RX credit bitmap, bits 31:1; replace on write, clear serviced bits on success |
| `0x1054` | `PERIPHERAL_TX_REQUEST` | RW | `0` | Software TX credit bitmap, bits 31:1; replace on write, clear serviced bits on success |
| `0x10d0` | `CORE_IDLE_STATUS` | RO | `1` | Bit 0: no active channels or occupied worker contexts |
| `0x10e0` | `DESIGN_CAPABILITY_STATUS` | RO | `1` | Configured design capability; bits 3:0 report one interrupt destination |
| `0x10f0` | `CORE_CAPABILITY_STATUS0` | RO | `0x0a602258` | Fixed core configuration |
| `0x10f4` | `CORE_CAPABILITY_STATUS1` | RO | `0x00000f17` | Supported optional features; chain bit clears when chaining is disabled |

`CORE_CAPABILITY_STATUS1` sets bits 0 (watchdog), 1 (timeout), 2 (tokens),
4 (outstanding contexts), 8 (independent directions), 9 (peripheral transfers),
10 (command chain), and 11 (endian conversion). Timer capability bits describe
support even when the configured thresholds are zero.

## Interrupt sources

The raw, clear, enable, and masked-status registers use bits 12:0:

| Bit | Source | Meaning |
|---|---|---|
| 0 | `command_complete` | A successful command with SET_INT generated an event |
| 1 | `read_slverr` | Read target returned an error not classified as a decode error |
| 2 | `write_slverr` | Write target returned an error not classified as a decode error |
| 3 | `read_decerr` | Read decode error, malformed descriptor, or read protocol error |
| 4 | `write_decerr` | Write decode or protocol error |
| 5 | `fifo_overflow` | Internal FIFO overflow or adapter overflow report |
| 6 | `fifo_underflow` | Internal FIFO underflow or adapter underflow report |
| 7 | `read_data_timeout` | Read transaction timeout or read-data phase timeout |
| 8 | `read_address_timeout` | Read-address phase timeout |
| 9 | `write_response_timeout` | Write transaction timeout or write-response phase timeout |
| 10 | `write_data_timeout` | Write-data phase timeout |
| 11 | `write_address_timeout` | Write-address phase timeout |
| 12 | `watchdog_timeout` | Enabled channel made no progress within its watchdog interval |

Writes to raw status set selected bits without generating completed-command
counts. Use this for software-visible interrupt injection. Clear error bits with
write-one-clear. Clear bit 0 once per queued event; IRQ stays high if any enabled
source remains pending.

Data-bus `TLM_ADDRESS_ERROR_RESPONSE`, `TLM_BURST_ERROR_RESPONSE`, and
`TLM_COMMAND_ERROR_RESPONSE` are classified as decode errors. Other non-OK
responses are classified as slave errors. A reported response ID other than 4,
or an adapter report of RLAST, write-credit, write-strobe, or multiple-return
faults, sets the corresponding read/write protocol error bit. These observations
require a target or bus adapter using `TransferExtension`.
