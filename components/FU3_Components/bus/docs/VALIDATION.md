# FU3 bus validation

Documentation reconstructed on 2026-10-17. No executable regression was rerun
as part of this documentation-only restoration.

## Recorded execution evidence

The retained local `build/sysbus-upgrade-final.log` records the following passing
runs. Its filesystem timestamp is 2026-10-16; this is historical evidence, not a
fresh certification of the current checkout.

| Build / use case | Passed CTest executions |
|---|---:|
| Static library regression | 5/5 |
| Static installed consumer | 4/4 |
| Shared library regression | 5/5 |
| Shared installed consumer | 4/4 |
| FX1/FU3 coexistence | 1/1 |
| Total | 19/19 |

These are repeated executions across build configurations, not 19 independent
specification requirements. The recorded environment is WSL/Linux with SystemC
2.3.4 and C++17. The retained `build/repo-shared-linkage.txt` resolves the installed
`libfu3_bus_system.so` and shared `libsystemc.so.2.3`. Generated logs are ignored
local artifacts and are not required in the repository.

## Coverage in the current test sources

| Test | Principal checks |
|---|---|
| [fu3_bus_config](../tests/bus_config_tests.cpp) | Default counts; invalid names, widths, ranges, capacities and overlapping windows; address bounds. |
| [fu3_bus_behavior](../tests/bus_behavior_tests.cpp) | Routing and local offsets; boundary rejection; APB splitting and timing; repeated strobes; downstream error/exception handling; debug image loading/readback; progress during denied traffic. |
| [fu3_bus_arbitration](../tests/bus_arbitration_tests.cpp) | FIFO service under flooding; independent output progress; queue recovery after target exceptions. |
| fu3_bus_zero_latency_fairness | The arbitration executable in zero-latency mode; runnable-master fairness. |
| [fu3_bus_contract](../tests/bus_contract_tests.cpp) | Independent golden address map; explicit metadata; fixed and incrementing footprints; narrow word adaptation; strobe handling; policy/handoff/reset guards; admission and actual downstream overlap; same-ID ordering. |
| [Installed consumers](../tests/installed_consumer/main.cpp) | Exported package usage and transactions over each of four target paths. |
| [Coexistence](../tests/coexist/CMakeLists.txt) | One executable links the FX1 and FU3 bus libraries together. |

CTest sets 30-second timeouts for regressions and 10-second timeouts for consumers
and coexistence. Behavioral/concurrency suites also contain simulation watchdogs.
Targets and traffic generators are test fixtures; their successful operation does
not verify the corresponding production IP.

## Reproduction and interpretation

Use the [README build and test commands](../README.md#build-and-test), which cover
static/shared builds, installation, consumers and optional FX1 coexistence without
ignored helper scripts. Stop on any failed command and retain the failing output.

The evidence supports the functional bus contract, bounded concurrency, routing
and package integration. It does not establish cycle-accurate timing, complete
AMBA conformance, CPU firmware execution, complete PMP/security enforcement,
native Windows DLL operation or production peripheral correctness.

Timer integration has separate evidence in the
[Timer validation record](../../timer/docs/VALIDATION.md). Its checks should not be
counted as a new execution of the standalone bus regression.

For assumptions and endpoint obligations, see [MODEL_CONTRACT.md](MODEL_CONTRACT.md).
