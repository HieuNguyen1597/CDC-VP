# FU3 WDT0/1 - SystemC/TLM

Behavioral watchdogs for software-visible timeout, warning interrupt and reset
recovery validation. One reusable `fu3_wdt::Watchdog` class serves both instances;
`WatchdogBank` creates WDT0 and WDT1 with independent state and configurable periods.

The model includes two-stage expiry, reload/service, raw/masked IRQ status, locked
register writes, partial byte writes, debug pause, separate reset domains and
programmable reset-request duration. Ambiguities in the source documents are
resolved explicitly in the [model contract](docs/MODEL_CONTRACT.md).

## Integration

```cpp
#include <fu3_wdt/wdt_bank.h>
fu3_wdt::WatchdogBank watchdogs{"watchdogs"};
// bus is an existing FU3 BusSystem with WDT0/1 enabled.
bus.target("WDT0").bind(watchdogs.watchdogs[0]->socket);
bus.target("WDT1").bind(watchdogs.watchdogs[1]->socket);
```

Bind every instance's `preset_n`, `wdogres_n`, `clock_enabled`, `debug_halt`,
`irq`, `reset_request` and `paused` ports before simulation. Route IRQ/reset outputs
in the platform; the library does not own CPU, PLIC or CRM behavior. Both windows
are on PERIBUS_0. Disable other absent bus targets before constructing the bus.
See [integration.cpp](tests/integration.cpp) for complete wiring with fake peers.

Firmware-style sequence: write the unlock key, program LOAD, set CONTROL.INTEN
and RESEN as needed, then lock. Service by unlocking, writing LOAD, and locking.
After a warning, use CLEAR to acknowledge raw interrupt and restart the counter;
LOAD alone does not clear raw status. Constants are in
[registers.h](include/fu3_wdt/registers.h).

## Build, test and install

Run from this directory in Linux/WSL with CMake and a compatible SystemC 2.3.4
installation. Both the library and consumers use the imported SystemC target and
C++17. Executables define `sc_main`; libraries do not define an entry point.

```bash
set -e
export SYSTEMC_HOME=/absolute/path/to/systemc-install
for mode in static shared; do
    shared=OFF
    if [ "$mode" = shared ]; then shared=ON; fi
    cmake -S . -B "build/$mode" -DSYSTEMC_HOME="$SYSTEMC_HOME" -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS="$shared"
    cmake --build "build/$mode" --parallel 4
    ctest --test-dir "build/$mode" --output-on-failure
    cmake --install "build/$mode" --prefix "$PWD/build/$mode/install"
    cmake -S tests/consumer -B "build/$mode/consumer" -DCMAKE_PREFIX_PATH="$PWD/build/$mode/install;$SYSTEMC_HOME"
    cmake --build "build/$mode/consumer" --parallel 4
    ctest --test-dir "build/$mode/consumer" --output-on-failure
done
```

Consumers use `find_package(fu3-wdt CONFIG REQUIRED)` and link `fu3::wdt`.
The production library does not depend on the bus library; the integration test
links both. See [traceability](docs/TRACEABILITY.md) and
[validation](docs/VALIDATION.md) for coverage and execution evidence.
