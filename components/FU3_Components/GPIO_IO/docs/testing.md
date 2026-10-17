# Testing

The suite runs on the host with SystemC; no board, FPGA or firmware is required.
CTest names use the prefix `fu3_gpio_` followed by the suffix below.

## Test scenarios

| CTest suffix | Behavior checked |
| --- | --- |
| registers | Three-bank reset maps, masks, ignored RO writes, walking data/direction, pad fields, isolation |
| sampling | Exact three-stage pipeline in each bank; input independent of direction/output |
| interrupts | Three banks × 32 pins × four modes; assertion cycle, RC, level reassertion, no edge repetition; simultaneous vector, frozen status, coincident lost edge |
| wake | Three banks × 32 bits; raw assertion/deassertion, priority, board OR, IRQ independence |
| protocol | Aliases; invalid command, byte enables, lengths, streaming, null pointer; debug; delay |
| reset | Asynchronous restoration with pending IRQ; reset-time rejection; release sampling |
| concurrency | Same-bank serialization; reset abort/recovery; different banks at same edge; reset releases pending and mutex-queued callers |
| reset_pulse | A reset pulse ending before mutex release invalidates both queued writes; new accesses recover |
| iosel | Package counts, codes, fixed functions, input-only cells, test decoder and suppression |
| integration | Independent 24-pin map; data/direction, package input → PADIN/IRQ/wake, mux disconnection |
| random | 5,400 seeded cycles; three per-pin reference machines; 48 registers and IRQ/sync/wake after every access |
| spec_register_outputs | Direct 32-bit data/direction ports; all 64 values for each of 96 PADCFG fields; neighbors and banks preserved |
| spec_input_stream | Consecutive walking/random inputs; pipeline; unsampled sub-cycle and sampled one-cycle pulses |
| spec_irq_boundaries | Empty RC/event coincidence, ignored W1C, invalid reads, pending reconfiguration, old-enable semantics, per-pin masking, disabled edges, mixed modes, opposite edges |
| spec_wake_priority | Every highest-selected pin in each bank; selected/unselected inputs; stopped clock; isolation; zero mask |
| spec_clock_reset | No pre-edge write; stopped-clock retention/raw wake; access abort; full reset without clock; held reset over edges; release pipeline |
| spec_pad_truth | Independent pin table; four codes × 32 data/enable/input combinations × two test-mode states × 80 positions; recovery, POR/straps/oscillator |
| spec_pad_random | 1,800 seeded vectors over 80 positions and both logical partitions; independent simultaneous changes; stopped GPIO clock |
| spec_address_decode | Writable words except INTEN with four low-bit × four upper-address aliases per bank; aliased status RC |

The GPIO reference model evaluates each pin independently of the implementation.
Expected package maps are independent of `pin_info`. The direct-core fixture
observes all 32 bits per bank; the standalone IO_PAD fixture drives primary and
alternate data and requested-enable inputs independently.

## Run the suite

Build using the [README commands](../README.md#build-and-test), then run these
commands from the repository root in Linux/WSL:

```sh
# Run all 19 scenarios
ctest --test-dir build/fu3-gpio-full --output-on-failure

# List scenarios without running them
ctest --test-dir build/fu3-gpio-full -N

# Inspect an individual scenario
ctest --test-dir build/fu3-gpio-full -R '^fu3_gpio_interrupts$' -V

# Run the eight additional spec scenarios
ctest --test-dir build/fu3-gpio-full -L spec --output-on-failure

# Run either fixture directly
./build/fu3-gpio-full/test_fu3_gpio_io reset_pulse
./build/fu3-gpio-full/test_fu3_gpio_spec pad_truth
```

Rebuild after changing code or tests. A successful complete run ends with:

```text
100% tests passed, 0 tests failed out of 19
```

Failures report the scenario, simulation time and assertion message. Use `-V`
to inspect details. Each scenario has a 30-second timeout. If CTest finds no
tests, check the build directory and `FU3_GPIO_BUILD_TESTS=ON`.

Without an argument, `test_fu3_gpio_io` runs `registers` and
`test_fu3_gpio_spec` runs `irq_boundaries`; neither runs the complete suite.
Direct spec-binary arguments omit the `spec_` prefix shown in CTest names.

## Test sources and helpers

- [test_gpio_io.cpp](../tests/test_gpio_io.cpp): subsystem integration, eleven scenarios.
- [test_spec.cpp](../tests/test_spec.cpp): direct GPIO and standalone IO_PAD, eight scenarios.

The subsystem fixture provides these helpers:

| Helper | Purpose |
| --- | --- |
| `wr(bank, offset, value)` | Write a register; bank 0/1/2 means A/B/C. |
| `rd(bank, offset)` | Normal TLM read, including INTSTATUS side effects. |
| `peek(bank, offset)` | Non-destructive debug read. |
| `raw(bank, value)` | Drive GPIO inputs through the fixture wiring. |
| `tick(n)` | Wait for n rising edges and allow signals to settle. |
| `check(condition, message)` | Record an assertion result. |

For example, this checks the GPIOA output latch; `integration` also checks
routing to the package pin:

```cpp
wr(0, reg::PADOUT, 1);
check(rd(0, reg::PADOUT) == 1, "GPIOA PADOUT bit0");
```

## Optional instrumentation

Enable GCC coverage by reconfiguring the build:

```sh
cmake -S components/FU3_Components/GPIO_IO -B build/fu3-gpio-full \
  -DFU3_GPIO_COVERAGE=ON
cmake --build build/fu3-gpio-full -j4
ctest --test-dir build/fu3-gpio-full --output-on-failure
cd build/fu3-gpio-full
gcov -b CMakeFiles/fu3_gpio_io.dir/src/gpio.cpp.gcda
gcov -b CMakeFiles/fu3_gpio_io.dir/src/io_pad.cpp.gcda
gcov -b CMakeFiles/fu3_gpio_io.dir/src/subsystem.cpp.gcda
```

For a separate UBSan build, run from the repository root:

```sh
cmake -S components/FU3_Components/GPIO_IO -B build/fu3-gpio-ubsan \
  -DSYSTEMC_INCLUDE_DIR=/opt/systemc-2.3.4/include \
  -DSYSTEMC_LIBRARY=/opt/systemc-2.3.4/lib/libsystemc.so \
  -DCMAKE_BUILD_TYPE=Debug -DFU3_GPIO_BUILD_TESTS=ON \
  -DFU3_GPIO_COVERAGE=OFF \
  '-DCMAKE_CXX_FLAGS=-fsanitize=undefined -fno-sanitize-recover=undefined -Wall -Wextra -Wpedantic -Werror'
cmake --build build/fu3-gpio-ubsan -j4
ctest --test-dir build/fu3-gpio-ubsan --output-on-failure
```
