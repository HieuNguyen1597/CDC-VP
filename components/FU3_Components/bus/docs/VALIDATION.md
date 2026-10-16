# Validation — 2026-10-16

Environment: Ubuntu through WSL, GNU C++ 15.2.0, C++17, installed SystemC
2.3.4 shared library. All generated files are under `bus/build/`.

Command: `SYSTEMC_HOME=<installed-prefix> bash scripts/build.sh`.

| Build/check | Result |
|---|---|
| Static bus regression | 5/5 CTest tests passed |
| Installed static consumer, each of four minimal paths | 4/4 passed |
| Shared bus regression | 5/5 passed |
| Installed shared consumer, each of four minimal paths | 4/4 passed |
| Link/run with FX1 bus in the same build | 1/1 passed |

Total: 19/19 CTest executions passed. Each regression build reports:

- Configuration: 20 checks, 0 failures.
- Behavior: 1,039 checks, 0 failures.
- Contention and exception recovery: 956 checks, 0 failures.
- Zero-latency contention: 796 checks, 0 failures.
- CPU/DMA and independent HAS contract: 486 checks, 0 failures.

The contract suite checks all 36 physical windows against an independent map,
requires request metadata, verifies narrow FIXED/INCR transfers and byte strobes,
rejects malformed requests and cross-window bursts before side effects, and
saturates CPU/DBG/DMA admission limits. The updated suite also measures actual
concurrent calls inside delayed fake IPs: CPU 2, DBG 1, DMA 4 per direction;
DMA read/write overlap across SRAM targets; AES 4 and QSPI 1 concurrently behind
AP_SB0. Same ordering ID remains serialized. It rejects DBG narrow/fixed requests,
checks aligned word adaptation and single-beat metadata at QSPI, preserves disabled
read lanes, and verifies narrow SRAM read/write without RMW. It also tests staging closure with pending
IBEX traffic, quiescent reset, and reopening staging for reboot. The behavior
suite includes the explicit AP/PP stage delays and exception recovery.

Behavior includes a continuously denied ROM requester and a second master
scheduled 1 ns later; the second master completes before the requester exhausts
its 100 attempts. Contention tests use eight masters repeatedly requesting the
same output, exercise all four paths, and check independent output progress.

The previously recorded `ldd` check on the installed shared consumer resolves both `libfu3_bus_system.so`
from the install tree and `libsystemc.so.2.3` from the SystemC installation;
no missing dependency. All test programs define `sc_main`, not `main`.

Local evidence (ignored by Git):

- `build/sysbus-upgrade-final.log`: complete final build/install/CTest run, exit 0.
- `build/repo-shared-linkage.txt`: shared dependency resolution.
- `build/{static,shared}/Testing/Temporary/LastTest.log`: detailed assertions.
- `build/{static,shared}/consumer/Testing/Temporary/LastTest.log`: installed tests.
- `build/coexist/Testing/Temporary/LastTest.log`: FX1/FU3 coexistence test.

This verifies the functional bus and packaging, not RTL cycle equivalence,
native Windows DLL support, full CPU firmware execution, or a complete FU3 VP.
No parent project integration or FX1 source modifications were made. The final
SYSBUS upgrade run completed with exit 0; no real peripheral IP is needed for
these tests. All fixtures remain in tests/support and are excluded from install.
