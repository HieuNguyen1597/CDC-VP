# FU3 bus repository handoff

## Deliverable

Standalone SystemC/TLM-2.0 library `fu3::bus_system`, with headers under
`fu3_bus/`, implementation, CMake install/export, reproducible build scripts,
behavioral regressions and an independent HAS address-map test. No FX1 source
or parent platform/CMake modifications are required to build this directory.

The current change adds explicit 1/2/4-byte INCR/FIXED transaction metadata,
mandatory metadata on default FU3 ports, bounded admission, per-ID ordering,
one-stage dispatch pipelines with independent downstream occupancy and quiescent boot-policy transitions. These
are public interface changes: existing CPU/DMA callers must attach `Access`
(prefer `ScopedAccess`) and follow README.md. A generic TLM payload alone is
no longer sufficient on the four default FU3 initiator ports.

## Reproduce

```sh
SYSTEMC_HOME=/absolute/path/to/systemc-install bash scripts/build.sh
```

Use a SystemC installation with compatible C++ ABI. The script tests static and
shared bus libraries, installed-package consumers and coexistence with the FX1
component in this repository. For standalone use without the FX1 directory, use
the direct CMake commands in README.md instead of the coexistence script step.
All executable entry points are `sc_main`; SystemC is a PUBLIC imported-target
dependency. No host-specific SystemC installation path is stored in source.

## Integration responsibilities

- Bind all enabled target ports; disable absent IPs before constructing the bus.
- CPU adapters distinguish instruction fetch from data. DMA adapters provide
  beat width, fixed/incrementing address progression, data length and byte enables
  explicitly. These are functional requests, not raw AXI bursts. Assign different
  `ordering_id` values only to independent streams that may complete out of order.
- Stop/drain IBEX before `finish_staging()`, then release CVA6 reset in the
  platform. Stop/drain all masters before `reset()` and reset external IPs there.
- Implement actual CSR semantics, AP_PLIC compact offsets, interrupts, clock
  gating and HyperRAM initialization in the corresponding IP/platform models.
- Use debug transport only for intentional loader/debug backdoors.

## Review boundary

This is a functional bus release, not a complete firmware-running VP or an AXI
cycle-accurate/performance model. Admission limits are bounded queued requests;
actual fake-IP overlap is tested, but individual AXI channels are not modeled.
Word-only interfaces receive aligned word operations and lane enables. Targets
with configured capacity above one must support reentrant blocking transport.
Remaining HAS ambiguities and model choices are enumerated in MODEL_CONTRACT.md.
VALIDATION.md records the actual completed validation, including shared linkage.

Review/add only this directory's source, tests, scripts, CMake files and docs.
`build/` contains ignored machine-local binaries/logs; the source PDF and its
extracted text/images are outside this package and must not be added with it.
No commit, push or staging operation was performed by this task.
