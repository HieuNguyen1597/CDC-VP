# Testing

The default build has **44 CTest cases**: 18 controller/component, 17 boundary
and parameter, eight flash, and one real FU3 bus integration case. Tests use
SystemC 2.3.4/C++17; see the [README build commands](../README.md#build-and-test).

## Run

```sh
ctest --test-dir build/fu3-qspi --output-on-failure
ctest --test-dir build/fu3-qspi -L '^p0$' --output-on-failure
ctest --test-dir build/fu3-qspi -L '^p1$' --output-on-failure
ctest --test-dir build/fu3-qspi -L corners --output-on-failure
ctest --test-dir build/fu3-qspi -L flash --output-on-failure
ctest --test-dir build/fu3-qspi -R '^fu3_qspi_p0_irq_w1c$' -V
```

P0/P1 selection contains 18 cases: six P0 and twelve P1. Each test prints an
assertion count and simulated end time. Assertions remain enabled in Release.
CTest timeout is 30 host seconds per case. Simulation watchdogs are 50 ms for
the original controller fixture, 100 ms for corner/flash, and 1 ms for bus.
`flash_image` creates and removes a temporary binary in its build directory.

## Source and spec mapping

**R** = `QSPI_Register_Description.pdf` (17 pages), **W** =
`QSPI_Wrapper_Interface.pdf` (9 pages). Numbers are PDF pages. All test names
have the prefix `fu3_qspi_`. The tables describe checked cases, not exhaustive
coverage; functional checks do not verify pin-level bus/pad or RTL CDC behavior.
See [model choices](../README.md#model-choices) for unresolved source wording.

| Test suffix | Checked behavior | Source / scope |
| --- | --- | --- |
| registers, components | 16 reset values, field/access masks, independent component APIs, IRQ source masks | R section 1 and 1.1-1.16, pp.2-14; map 1.17, pp.15-17. Invalid RO/reserved writes follow model policy. |
| protocol, errors | Word access, malformed payloads, illegal divider/poll length, bounds, BUSY-protected fields | R section 1 p.2, 1.1 p.2, 1.2 p.4, 1.5-1.6 pp.6-8; W 3.2-3.3 pp.4-5. Exact TLM error codes/ADSIZE overflow rejection are model policy. |
| triggers, indirect | CCR/AR/DR start, DLR+1/undefined length, byte order and streaming | R 1.5-1.7 pp.6-9, 1.9 p.10; partial-word packing is a model convention. |
| fifo, threshold_words | Levels, thresholds, full/empty, overrun/underrun | R 1.1 p.3 versus 1.3 p.5 has conflicting units; 1.15-1.16 pp.13-14 defines errors/clear. Both threshold profiles tested. |
| polling, timeout | AND/OR, APMS, mask/match, last status, PTR finishes current read and is not XIP inactivity | R 1.1 pp.2-4, 1.9-1.13 pp.10-12; empty-mask and stop priority follow model policy. |
| dma | Request/clear, word thresholds, not-all-written TX condition, live enable, abort/tails | R 1.14 pp.12-13; W 3.5 p.5. Partial RX word and prefill/rearm are model conventions. |
| phases, spec_timing | SDR/DDR phase descriptors, 24 width/size/rate combinations, SIOO, minimum PIR/CSHT | R 1.1 p.2, 1.2 p.4, 1.6-1.8 pp.7-9, 1.12 p.11. Timing lower bounds/metadata, not waveform equivalence. |
| xip | Mapped word reads, FSIZE, AR ignored, DR zero and backend errors | W 3.3 pp.4-5; R 1.2 p.4, 1.7 p.9, 1.9 p.10. Rejecting mapped writes is model policy. |
| reset, clock, concurrency | Three reset domains, gated core, bus timeout and queued-caller cancellation | W 1.1 p.3, 3.1 p.4, table 2-1 p.4; isolated reset ownership/admission cleanup are model conventions. |
| random | 100 indirect transfers and 100 XIP reads | Seeded regression, not a separate FU3 requirement. |
| bus | CVA6 INCR fetch, DMA FIXED DR access, program -> WIP polling -> XIP, erase, QE/SIOO | Existing FU3 bus map/transport plus controller and generic NOR composition. No CPU instruction execution. |

## Additional P0/P1 cases

| Priority / test suffix | Cases | Source / scope |
| --- | --- | --- |
| P0 p0_triggers | 256 EN/FMODE/ADMODE/DMODE/instruction combinations; correct stage and one frame | R 1.6 p.7, 1.7 p.9, 1.9 p.10; no-data polling rejection is model policy. |
| P0 p0_dlr | 1/2/3/4/5/31/32/33 bytes, minimum FSIZE, 0xfffffffe/0xffffffff, 4-GiB bounds | R 1.5 pp.6-7, 1.2 p.4. Huge legal transfers inspect one bounded chunk then abort; no 4-GiB stream/allocation. |
| P0 p0_fifo | Packing through 65 bytes, RX byte-33 stall, full TX discard/retry, byte/frame audit | R 1.3 p.5, 1.9 p.10, 1.15 p.13; partial-word zero fill is model convention. |
| P0 p0_irq_w1c | Persistent TX/RX threshold clear/reassert, simultaneous flags, mask/unmask, zero writes, FCR vs DMAFCR, event vs level | R 1.1 p.3, 1.3 p.5, 1.4 p.6, 1.15-1.16 pp.13-14. |
| P0 p0_active_reset | Serial prefix, TX starved, RX full/unread, polling gap, XIP latency, AP queued/annotated callers | W 1.1 p.3, 3.1 p.4; reset cancellation/retention are model conventions. |
| P0 p0_bus_errors | Address/size/strobes, null/command errors, aperture/FSIZE, backend error/exception, recovery, payload preservation | R section 1 p.2, 1.2 p.4; W 3.2-3.3 pp.4-5. TLM policies, no PSLVERR/HRESP pin waveform. |
| P1 p1_polling | Status lengths 1..4, AND/OR/APMS, masked bytes, empty masks, PTR 0/1/31/32, stop priority, max PIR | R 1.1 pp.2-4, 1.9-1.13 pp.10-12; empty-mask/priority are model policy. |
| P1 p1_dma | Thresholds 0/1/7/8/31, held clear, live disable/reenable, abort, seven-byte tail, one-byte TX remainder | R 1.14 pp.12-13; W 3.5 p.5; tail/prefill conventions as above. |
| P1 p1_domains | Ordered XIP while AP runs, PCLK/HCLK gating isolation, core-stop timeouts and stale-write prevention | W 1.1 p.3/table 2-1 p.4 plus model admission contract. |
| P1 p1_parameters_* | Capacities 1/3/4/7/16/32, TX/RX 4/7 and 7/4, different clocks, short tails and response timeout | W table 2-1 pp.3-4, R 1.3 p.5; capacities in bytes under selected profile. |
| P1 flash_integrity | Full page over 64 chunks, foreign begin/cancel, replay, 12 descriptor mutations, direction/order/end faults and recovery | FlashTransaction API and generic NOR profile, not FU3 controller spec. |

`tests/test_corners.cpp` uses a sparse virtual 4-GiB target for boundary/error
injection and host byte vectors for packing oracles. Parameter tests use eight
capacity/clock configurations; subword-capacity word rejection is explicit.
`tests/test_flash.cpp` tests the reusable NOR backend: `flash_commands`,
`flash_program`, `flash_erase`, `flash_protocol`, `flash_reset`, `flash_image`,
`flash_sioo`, `flash_integrity`. These are support-profile checks rather than
additional requirements from the two FU3 PDFs.

## UBSan and optional reports

```sh
cmake -S components/FU3_Components/QSPI -B build/fu3-qspi-ubsan \
  -DSYSTEMC_HOME=/opt/systemc-2.3.4 -DCMAKE_BUILD_TYPE=Debug \
  -DFU3_QSPI_BUILD_TESTS=ON \
  '-DCMAKE_CXX_FLAGS=-fsanitize=undefined -fno-sanitize-recover=undefined -Wall -Wextra -Wpedantic -Werror'
cmake --build build/fu3-qspi-ubsan -j4
ctest --test-dir build/fu3-qspi-ubsan --output-on-failure
python3 components/FU3_Components/QSPI/scripts/record_tests.py \
  build/fu3-qspi build/fu3-qspi-ubsan
```

The optional recorder runs both suites, verifies the 44-case set and UBSan
flags, and records test results/assertions, toolchain and source hashes.
Generated transcripts/JSON go to `build/fu3-qspi/test-reports/`, outside docs
and ignored by Git. Override with `--output-dir PATH`. Normal CTest also keeps
its last output under each build's `Testing/Temporary/LastTest.log`.

The source does not resolve byte/word thresholds, isolated-reset retention,
mapped writes, EN-disable FIFO handling, empty masks or simultaneous stop
priority. No full-spec coverage percentage or RTL-equivalence claim is made.
