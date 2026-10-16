# FU3 bus functional contract

Source: FU3 HAS v2.0, pp. 21, 24–31, 44–47; DMA pp. 15–16;
boot/access pp. 41–45, 62–63. Source PDF stays local and is not packaged.

## Address decoding

`src/config.cpp`, `BusConfig::fu3()` is the single address-map definition used
by the library. All 36 descriptors are enabled by default. IP register maps
are owned by their models, not the fabric.

| Target | Base | Size | Path |
|---|---|---|---|
| ROM | 0x00000000 | 4 KiB | SYSBUS_1 |
| CVA6_ISRAM | 0x00010000 | 64 KiB | SYSBUS_1 |
| IBEX_ISRAM | 0x00020000 | 64 KiB | SYSBUS_1 |
| DSRAM | 0x01000000 | 64 KiB | SYSBUS_1 |
| IBEX_DBG | 0x1A110000 | 4 KiB | SYSBUS_1 |
| QSPI_XIP | 0x30000000 | 128 KiB | SYSBUS_0 |
| EDHOC_OSCORE | 0x30100000 | 64 KiB | SYSBUS_0 |
| AES | 0x40000000 | 64 KiB | SYSBUS_0 |
| HYPERBUS | 0x60000000 | 32 MiB | SYSBUS_1 |

Each ISRAM reserves its final 4 KiB for debug code. Fabric maps each SRAM as
one physical target; the firmware layout must preserve the reservation.

Every following target has a 64 KiB aperture:

| PERIBUS_0 target | Base | PERIBUS_1 target | Base |
|---|---|---|---|
| AP_QSPIC | 0x44000000 | AP_PLIC | 0x40100000 |
| SYSCSR | 0x44010000 | CRM | 0x50010000 |
| AP_DMA | 0x44020000 | MAILBOX_CVA6 | 0x50020000 |
| RNG | 0x44030000 | GPIOA | 0x50030000 |
| MAILBOX_IBEX | 0x44040000 | GPIOB | 0x50040000 |
| WDT0 | 0x44050000 | UART2 | 0x50050000 |
| TIMER0 | 0x44060000 | TIMER2 | 0x50060000 |
| TIMER1 | 0x44070000 | TIMER3 | 0x50070000 |
| WDT1 | 0x44080000 | I2C | 0x50080000 |
| SPI1 | 0x440B0000 | SPI2 | 0x50090000 |
| SPI0 | 0x440C0000 | ADC | 0x500A0000 |
| UART0 | 0x440D0000 | AP_HBCTRL | 0x500B0000 |
| UART1 | 0x440E0000 | SPI3 | 0x500C0000 |
| GPIOC | 0x440F0000 | — | — |

All holes are unmapped, including 0x4409xxxx/0x440Axxxx, the old AP_HBCTRL
window 0x4410xxxx, removed PLIC alias 0x5000xxxx, and 0x500D0000–0x5010FFFF.
Exact windows are propagated through routers; no broad mask aliases PLIC into
the 0x500x cluster. AP_PLIC receives a local byte offset; compact REG_BUS register
translation remains the responsibility of its future target adapter.

## Functional acceptance boundary

This deliverable is a reusable SystemC/TLM bus component. CPU/DMA adapters obey
`Access`; `ScopedAccess` provides safe attachment/restoration. Real memory, CSR,
IRQ, reset/clock controllers and CPU firmware execution belong to the platform.

| Requirement / source | Implementation and evidence |
|---|---|
| Two SYSBUS, two A2P/PERIBUS; pp.21,25 | Both bridges under SYSBUS_0; routing and timing tests |
| Address map pp.46-47 | 36 windows / 38 rows (SRAM image/debug pairs); independent golden table and routing tests |
| DMA 8/16/32-bit, fixed/incrementing, 16 beats pp.15-16 | Explicit width/mode, complete footprint check, narrow/FIFO/strobe tests |
| CPU 2, DBG 1, DMA 4 outstanding pp.27-28 | Bounded admission, independent ordering IDs and DMA read/write gates; actual fake-IP concurrency tests |
| AP/PP one stage at TOP p.26 | FIFO timed dispatch stages; SYSBUS stage releases before downstream completion |
| Table 7-1 interface constraints | DBG word/INCR restriction; aligned word adaptation; SRAM/AES capacity 4, ROM/QSPI capacity 1 |
| ROM, staging, DSRAM policy pp.41-45 | Mandatory metadata, ROM restrictions, no DSRAM fetch, quiescent handoff/reset tests |
| Reserved unmapped p.25 | Exact windows, errors before side effects, old aliases rejected |
| AP_PLIC global decode pp.26-28 | Distinct 0x40100000 route to PERIBUS_1, local byte offsets to adapter |

## Confirmed strobe interpretation

User clarification (2026-10-16): `Strobe = Full` in the FU3 interface table means
full support for byte write strobes, including partial and sparse writes within
valid byte lanes. It does not require every write to enable all four bytes.
Bus and target adapters must preserve enabled lanes and leave disabled lanes
unwritten. They must not force all strobes high or emulate partial writes using
CSR read-modify-write. Existing functional byte-enable handling implements this
choice; the partial-write regressions remain applicable.

This resolves the meaning of `Full`, including the functional requirement at
APB endpoints so marked. Transfer-size adaptation remains a separate decision. TLM read byte enables select returned data in this
model; they do not represent a hardware read-strobe signal.

## Confirmed APB version

The user requires both peripheral buses to use the same APB version. Since
PERIBUS_1 is explicitly APB4 in the HAS, the implementation contract selects
APB4 for both PERIBUS_0 and PERIBUS_1. This is a user-confirmed clarification of
the generic APB label on PERIBUS_0, not a claim that the PDF explicitly labels
both buses APB4. Both A2P paths retain their existing topology and address map.
Full byte-write-strobe support applies to both paths. The shared functional
ApbBridge implementation already handles byte enables; no signal-level APB
handshake model is added by this decision. Subword read/CSR side-effect policy
remains separate and is not resolved by choosing APB4.

## Confirmed FIFO arbitration

User decision: use FIFO because the HAS does not specify priority levels.
At each contended output, requests are dispatched in arrival order at that
arbiter after incoming delay is consumed. There is no fixed master priority,
QoS override or round-robin selection. Same-time arrivals follow SystemC
scheduler enqueue order. Arbitration is per dispatched beat; a master issuing
continuously cannot jump ahead of already queued beats.

FIFO is local to a contention point, not one global queue across independent
outputs. Different targets can progress and finish independently; FIFO dispatch
does not force global response completion order. The existing same-stream
ordering contract below remains in force.

## Outstanding limits: PDF recheck and selected defaults

The user authorizes choosing VP defaults where the HAS is silent. The PDF does
specify many limits; a dash is not interpreted as unlimited capacity.

| Interface | HAS evidence | Selected functional limit |
|---|---|---|
| CVA6, Ibex | Table 7-1 p.27: 2, preserve adapter limits | 2 total read/write requests per CPU port |
| DBG master | Table 7-1 p.27: 1 | 1 total |
| DMA0 master | Table 7-1 p.28: 4; p.16 explicitly per independent engine | 4 reads plus 4 writes |
| ROM target | Table 7-1 p.28: 1 | 1 total |
| ISRAM, DSRAM, AES targets | Table 7-1 p.28: 4 | 4 total per target |
| A2H_QSPI target | Table 7-1 p.28: 1 | 1 total |
| APB peripheral targets, including PLIC | Table 7-1 pp.27-30: 1 | 1 per target; each A2P path serialized |
| HYPERBUS target | Table 7-1 p.30: dash | 1 total, configurable VP default |
| A2TL / EDHOC target | Table 7-1 p.31: dash | 1 total, configurable VP default |
| IBEX_DBG memory-map target | No separate target capacity established | 1 total, configurable VP default; distinct from DBG master |
| SYSBUS fabric | Table 7-1: dash | No invented global one-request limit; bounded master and leaf admission |

The CPU shared read/write limit is a conservative interpretation, not an explicit
per-direction statement from the PDF. Page 36 also gives CVA6 NrLoadBufEntries=2
and MaxOutstandingStores=2; these core resources do not unambiguously establish
external AXI read/write admission counts. Page 38 gives AxiIdWidth=2: ID width is
not itself the outstanding limit. Do not infer four outstanding requests from it.
The bus retains the interface table's bound and permits adapter-specific overrides
when the real CPU interface contract is available.

For unspecified target limits, one pending operation avoids assuming reentrant IP
support and preserves functional correctness; it can reduce throughput. Raise
TargetConfig::outstanding only with an adapter that supports concurrent calls.
Current defaults already match these choices, so this decision changes documents,
not runtime behavior. Same-ID serialization remains a separate conservative model
choice; admission capacity alone is not proof of same-ID AXI pipelining.

## Model choices and remaining external dependencies

- FIFO dispatch is confirmed by the user above; no master priority is assumed.
  `Access::ordering_id` identifies independent adapter ordering streams. Each
  master/direction/ID remains ordered; different IDs and read/write directions
  may overlap. Default ID zero preserves conservative ordering. This token is
  not a bit-accurate AXI ID port or a model of individual AXI channels.
- Admission includes ordered waiters and bounds whole upstream requests. SYSBUS
  dispatch stages release before downstream response, permitting independent
  outputs through AP_SB0 to progress concurrently. Leaf capacity defaults to one;
  FU3 ISRAM/DSRAM/AES allow four pending single beats. APB remains serialized.
  Custom targets must support their configured `outstanding` reentrant calls.
- Mandatory metadata distinguishes instruction fetch. Mislabeling fetch as data
  is an adapter error; the bus cannot infer CPU intent. Legacy custom ports must
  explicitly opt into this contract before being treated as FU3 masters.
- DMA has a 16-beat limit; CPU/DBG use a configurable VP cap of 256. Fixed and
  incrementing functional address progression are supported. WRAP, atomics and
  streaming wrap are unsupported. Requests must stay within one target. Ingress
  emits single incrementing operations, including repeated singles for
  fixed-address requests. This is NOT an assertion that DMA supports AXI FIXED
  bursts: pp.15-16 describe address progression, while Table 7-1 says INCR.
- DBG accepts only 4-byte incrementing requests. Interfaces marked size 32 in
  Table 7-1 receive aligned 4-byte operations: narrow accesses are lane-adapted
  with byte enables and read extraction, without read-modify-write. QSPI receives
  only single operations with at most one pending. For read-side-effect registers,
  the IP adapter must define word-read behavior. Full byte-write-strobe support is confirmed by the user above; real IP
  adapters must implement that contract.
- Narrow-transfer adaptation details and AXI error encoding remain unspecified;
  both peripheral buses use APB4 and support byte write strobes as confirmed above.
  Targets must honor functional byte enables; there is no CSR read-modify-write.
- Clock periods are configurable assumptions due to inconsistent PDF frequencies.
  AP/PP stage occupancy is represented; signal-level clocks, CGF and CDC are not.
- The PERIBUS_1 SYS column conflicts with both figures. The agreed topology uses
  both figures and preserves the published addresses.
- AP_PLIC compact register translation and A2TL target protocol remain target/IP
  responsibilities because the HAS does not supply their final definitions.
- DBG/DMA ROM denial is conservative; the full permission matrix, PMP/ePMP and
  CPU cacheability depend on the separate Common Spec/platform integration.
- Staging closure requires IBEX quiescence. Functional reset requires all-master
  quiescence, reopens staging, and neither aborts requests nor resets external IPs.
- Loader debug transport bypasses policy/timing but checks footprint. Firmware-byte
  readback is tested; this package is not an ELF parser or an instruction simulator.

## SYSBUS delivery scope (2026-10-16)

The current task owns SYSBUS_1/SYSBUS_0 routing, arbitration, functional AP stages,
master/target occupancy, ordering and the bus-facing transfer contract. Existing
A2P/PERIBUS routes remain available and regression-tested; implementing unfinished
CPU, DMA, AES, QSPI, PLIC or other IP internals is not an acceptance prerequisite.
Tests bind sparse fake IPs to all enabled windows. They inject response latency,
errors and exceptions and record actual arrival/occupancy. These fixtures are only
under `tests/`, never installed with the library.

The configured capacities are CPU 2 shared between read/write, DBG 1, DMA 4 per
direction; SRAM and AES target capacity is 4, ROM and QSPI 1. Other unspecified
AXI targets conservatively default to 1 and are configurable. Independent ordering
streams can overlap; preserving ID-zero ordering is intentionally the default.
No assertion is made that transaction timings predict hardware bandwidth.

The unresolved transfer-size details, A2TL protocol, PLIC local register map,
IP reset/clock behavior and full permission matrix remain integration contracts.
They are not fabricated implementations inside SYSBUS. For now, fake targets honor
the confirmed TLM byte-write-enable contract; real adapters must honor partial
writes for interfaces marked `Full`. Other unsupported operations must be rejected
explicitly. Register read side effects remain owned by those adapters.

## Regression evidence

`bus_contract_tests.cpp` independently transcribes all target windows, verifies
base/size/path and routes both boundaries through actual sockets. It covers missing
and malformed metadata, instruction protection, narrow FIXED/INCR transfers,
periodic strobes, preflight rejection, admission saturation, staging and reset.
Other suites cover delays, exception restoration, sustained-master fairness,
zero-latency operation and debug loading. Package checks exercise static/shared,
installed consumers and FX1 coexistence. See VALIDATION.md for completed runs.

Acceptance means the documented functional TLM contract is implemented and tested.
The external dependencies above are not silently implemented assumptions and do
not imply full RTL conformance.
