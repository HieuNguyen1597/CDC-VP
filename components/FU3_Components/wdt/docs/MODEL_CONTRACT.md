# WDT0/1 functional model contract

This SystemC/TLM model implements watchdog behavior for VP software, data-flow
and recovery-flow validation. It is not a signal-level APB or CDC implementation.
The user authorized resolution of the previously reviewed ambiguities before
implementation. The choices below are VP resolutions, not newly established spec facts.

## Evidence and decisions

References use PDF page numbers from WDT HDS V1.0 and FU3 HAS v2.0. Private PDFs
and extracted text remain excluded from the repository.

| Subject | Evidence / conflict | Selected behavior |
|---|---|---|
| Core counter | HDS pp.13, 21-22 | 32-bit downcounter; first expiry sets raw interrupt and TIMEOUT_SEEN and reloads. Second expiry clears TIMEOUT_SEEN and requests reset if RESEN. |
| Recovery | HDS pp.22, 27-28 vs HAS general description | LOAD reloads and clears TIMEOUT_SEEN. CLEAR clears raw and TIMEOUT_SEEN and reloads; timely service prevents escalation. |
| Raw status after LOAD | HDS p.12 says interrupt persists until CLEAR; p.22 only explicitly clears TIMEOUT_SEEN on LOAD | LOAD preserves existing raw interrupt. |
| Lock reset | HDS p.13 says unlocked; p.29 initialization says locked | Reset unlocked; LOCK reads 0 unlocked, 1 locked. Unlock key is 0x1ACCE551. The non-hex key printed on p.30 is treated as a typo. |
| Register addresses | HDS p.24 formula and table can double-count 0x1000 | Table offsets are relative to WDT_BASE directly. No extra 0x1000. |
| Pulse duration | HDS p.14 says unsupported; pp.26-27 define RESDIV and p.16 draws pulse divider | Implement detailed RESDIV table: encodings 0..8 mean 1..256 watchdog periods; 9..15 are stored but use one period. |
| Debug | HDS p.22 requires system debug plus DBGEN; p.32 tests DBGEN alone | Explicit debug_halt input; pause = DBGEN AND debug_halt. |
| Debug status reset | HDS p.20 and p.28 disagree | Status is derived from DBGEN and debug_halt, not independently latched. Programming reset clears DBGEN; watchdog-only reset does not. |
| Reset port direction | HDS p.17 diagram conflicts with pp.15, 20 | wdogres_n is active-low input; reset_request represents active-high wdogres output. |
| APB / strobes | HDS uses APB4 labels but lacks PSTRB detail; HAS Full strobe and user clarification | Functional APB4 endpoint with full byte-enable support; lane rules below. |
| SoC map | HAS map, rather than older HDS SoC diagram | WDT0 0x44050000 and WDT1 0x44080000, both PERIBUS_0, 64 KiB each. |
| IRQ integration | HAS text says WDT0 TBD but Ibex table p.52 gives NMI | Integration test follows the detailed table: WDT0 to Ibex NMI; WDT1 to PLIC source 18. Wiring belongs to the platform. |

## Register ABI

All values are little-endian. The public C-compatible constants are in
[registers.h](../include/fu3_wdt/registers.h).

| Offset | Register | Access | Reset / behavior |
|---|---|---|---|
| 0x1000 | LOCK | RW | Unlocked, reads 0; key opens protected writes, other active writes lock. |
| 0x1004 | LOAD | RW | 0xFFFFFFFF; active write reloads counter and clears TIMEOUT_SEEN. |
| 0x1008 | VALUE | RO | 0xFFFFFFFF on watchdog reset. |
| 0x100C | CONTROL | RW | 1 on programming reset; RESEN[0], INTEN[1], DBGEN[2], RESDIV[6:3]. |
| 0x1010 | RAW | RO | INT_GEN[0], TIMEOUT_SEEN[1]; zero on watchdog reset. |
| 0x1014 | MASKED | RO | raw interrupt AND INTEN in bit 0. |
| 0x1018 | CLEAR | WO | Any active write clears interrupt/seen and reloads; reads zero. |
| 0x101C | DEBUG | RO | Derived debug pause in bit 0. |

INTEN enables both the counter and IRQ output. Disabling it freezes count and
masks IRQ without clearing raw/seen. RESEN masks reset output. CONTROL reserved
bits read zero and ignore writes. No capability, identification, integration-test
or prescaler registers are invented from introductory references without an ABI.

Naturally aligned 1/2/4-byte accesses within a register are accepted. Byte enables
repeat over payload bytes and must be 0x00 or 0xFF. LOAD/CONTROL merge enabled
lanes internally, without bus read-modify-write. An all-disabled write has no
side effects. LOCK requires all four lanes and the full key in one write to unlock;
any other active write locks, preventing partial key accumulation. Any active lane
of CLEAR triggers acknowledgement, regardless of data. Locked protected writes
and RO writes succeed without changing state. These are explicit VP adapter rules.

Only the eight listed registers decode; there are no upper-address aliases.
Reserved/unmapped/misaligned accesses return ADDRESS_ERROR. Unsupported lengths
or streaming wrap return BURST_ERROR; invalid enables return BYTE_ENABLE_ERROR;
unsupported command returns COMMAND_ERROR; null data or access during either reset
returns GENERIC_ERROR. Errors complete after the configured access latency.

## Timeout timing and event priority

Watchdog tick and access latency are positive constructor parameters, defaulting
to 10 ns and 1 ns. Defaults are VP values, not chip frequencies. Bus A2P latency
is additional. Annotated input delay is consumed exactly once, with zero returned.

Ticks lie on a grid anchored at simulation time zero. Enabled count N expires
when its Nth eligible tick decrements 1 to 0; the count reloads on that tick.
Writing LOAD=0 immediately sets raw and TIMEOUT_SEEN even with INTEN disabled;
the next eligible tick is the second timeout. CLEAR with LOAD=0 clears state
immediately; its next eligible tick is a first timeout. Zero reload never causes
an infinite same-time loop.

The model advances elapsed ticks before committing an access at its completion
time. Thus expiry wins over a write completed at exactly the same tick. Service
cannot retract a reset pulse already emitted. Simultaneous callers commit in
SystemC scheduling order. Writes are atomic, with no wait inside state mutation.
Reset assertion overrides state in its own domain at that timestamp. Outputs
settle in subsequent delta cycles through one writer process.

Counter progress uses arithmetic and schedules only the next expiry, reset-pulse
end or input change. Long quiet countdowns do not generate one kernel wakeup per
tick. Clock gating/debug/INTEN freeze the count without changing the global grid.
There is no runtime clock-frequency API. Reset pulses use elapsed watchdog-period
duration, independent of counter pause/gating. A later second-timeout event starts
a new pulse interval using the current RESDIV, replacing the previous deadline.
RESEN masks an existing pulse; re-enabling during its interval exposes the
remaining pulse. A RESDIV write alone does not change an already issued pulse.

## Reset and platform boundary

`preset_n` restores LOAD, CONTROL and LOCK and prevents counting while asserted.
It does not independently clear raw/seen/VALUE. `wdogres_n` resets VALUE to
0xFFFFFFFF and clears raw, seen and any reset pulse, retaining programmed
configuration and lock. After watchdog-only reset, enabled counting starts from
0xFFFFFFFF, not LOAD, following the documented VALUE reset. Software may service
the watchdog again to select the configured interval.

`reset_request` is only a request; the model does not reset a CPU, itself or another
IP automatically. The platform/CRM owns reset scope and feedback. Integration
tests use fake receivers and per-instance watchdog-reset feedback; this does not
establish production CRM whole-system reset ownership. No CPU firmware boot has
been executed by these tests.

`transport_dbg` supports untimed, side-effect-free register reads (with elapsed
counter time accounted for); debug writes are rejected. DMI and nonblocking
transport are unsupported. Blocking accesses must originate from SC_THREADs.
