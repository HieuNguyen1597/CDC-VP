# WDT requirement traceability

Private source: WDT HDS V1.0; page references are PDF pages. Tests exercise the
selected [model contract](MODEL_CONTRACT.md), including clearly labelled resolutions.

| Requirement | Evidence | Verification |
|---|---|---|
| Downcount and two-stage timeout | pp.13, 21-22 | behavior: exact first/second deadlines, no early IRQ/reset, reload and seen status. |
| Service and acknowledgement | pp.22, 28, 30-31 | behavior: LOAD preserves raw, CLEAR removes it; integration: service WDT0 prevents escalation while WDT1 expires. |
| Counter/interrupt enable | pp.25-26 | behavior: disabled zero load produces raw only; re-enable exposes raw. |
| Write lock and key | pp.12-13, 24, 29-31 | behavior: full key, non-key, partial key, protected writes; integration: locked CLEAR leaves IRQ asserted. |
| Register decode and status | pp.23-28 | behavior: reset reads, RO/WO policy, reserved addresses, invalid accesses, byte lanes. |
| Debug halt | pp.12, 22 | behavior: pause/readback/resume; DBGEN alone permits progress. |
| Separate clocks and resets | pp.19-21 | behavior: gate, reset during activity, reset retention; integration: fake CRM watchdog-only reset feedback. |
| Reset duration | pp.26-27, conflicts with p.14 | behavior: default one tick and RESDIV=2 four ticks; explicit VP resolution. |
| Full strobe behavior | HAS interface table and user decision | behavior: sparse/repeating masks, byte/halfword, disabled lanes; integration: byte write through A2P. |
| WDT0/1 map and IRQ routes | HAS memory/interrupt tables | integration: real bus, independent state, fake Ibex NMI and PLIC18 signals. |
| Long timeout efficiency | VP implementation objective | behavior: one simulated second advances 100 million ticks arithmetically. |
| Reusable SystemC packaging | Project integration requirement | static/shared builds and installed behavior-test consumer. |

The fake CRM checks request/feedback behavior only. Whole-system reset scope,
CPU firmware execution, actual PLIC servicing and full boot flow require platform
integration. Physical CDC and pin-level APB protocol are outside this VP contract.
