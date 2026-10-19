# AP_PLIC validation - 2026-10-19

Environment: Ubuntu under WSL, GNU C++ 15.2.0, C++17, SystemC 2.3.4 shared installation.
Built with `-Wall -Wextra -Wpedantic`: no warnings.
Command: `tests/run_all.sh`.

| Check | Result |
| --- | --- |
| Static library: behavior and FU3 bus integration | 2/2 CTest tests passed |
| Installed static package consumer | 1/1 passed |
| Shared library: behavior and FU3 bus integration | 2/2 passed |
| Installed shared package consumer | 1/1 passed |

Total: 6/6 CTest executions passed.

- Behavior: 634 checks, zero failures.
- Integration: 631 checks, zero failures. The three malformed-access checks run only in the direct bench, because the FU3 bus rejects them first.

The integration bench uses the production FU3 bus with only the AP_PLIC window enabled and drives it from the CVA6 initiator with `fu3_bus::ScopedAccess`, so requests pass SYSBUS_1, SYSBUS_0, AP_PP1 and the A2P bridge before reaching local offsets.

Mutation checks, each making both benches fail: PRIORITY mask 0x3333_3333, `>=` instead of `>` in the threshold comparison, edge counter saturating at 9.
