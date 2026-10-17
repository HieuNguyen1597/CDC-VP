# WDT validation

Environment: WSL/Linux, GNU C++ 15.2.0, C++17, SystemC 2.3.4. Validation date:
2026-10-17. Reproduction commands are in the [README](../README.md#build-test-and-install).

## Execution results

| Configuration | Result |
|---|---|
| Static library: behavior and real-bus integration | 2/2 CTest passed |
| Static installed consumer | 1/1 CTest passed |
| Shared library: behavior and real-bus integration | 2/2 CTest passed |
| Shared installed consumer | 1/1 CTest passed |
| Total | 6/6 CTest executions passed |

Each behavior execution reports 221 checks and zero failures; each integration
execution reports 54 checks and zero failures. Installed consumers repeat the
221-check behavior suite using the exported package. These are repeated checks
across configurations, not six independent specification requirements.

The completed run is recorded locally in `build/validation.log`. No compiler
warnings or errors were reported. `ldd` on the shared installed consumer resolves
the installed `libfu3_wdt.so` and `libsystemc.so.2.3`; no dependency is missing.
The shared consumer links only the WDT public target, without a bus dependency.

## Test scope

- `wdt_behavior`: direct TLM register transactions, independently calculated
  timeout boundaries, first/second timeout status, reload/clear, raw/masked IRQ,
  write locking, partial and sparse lane writes, RO/WO policy, malformed requests,
  reset domains, debug pause, clock gating, zero-load escalation, reset pulse
  duration, simultaneous expiry/service and a long countdown.
- `wdt_integration`: both WDTs connected through the real FU3 bus and A2P,
  independent registers, byte writes, warning routing, locked acknowledgement,
  selective service, second-timeout requests and fake CRM reset feedback.
- `wdt_installed_consumer`: the behavior suite built against the installed package,
  linking only `fu3::wdt` and its exported SystemC dependency.

CTest timeouts are 30 seconds. Simulation watchdogs are two seconds for behavior
and 100 microseconds for integration. The long-count test advances one simulated
second (100 million watchdog ticks) without a per-tick simulation loop; it is a
functional scalability check, not a general boot-performance benchmark.

## Limits

Tests verify the selected VP contract, including the documented resolutions of
contradictory HDS passages. They do not establish physical CDC behavior, RTL cycle
accuracy, native Windows DLL support, production CRM reset scope, CPU firmware
execution or PLIC claim/complete behavior. The interrupt receiver and CRM are
test-only peers. WDT0's NMI route follows the detailed HAS table despite its
conflicting TBD prose.

Local generated logs and build artifacts remain under ignored `build/`. No private
PDFs or extracted specification text are part of this component.
