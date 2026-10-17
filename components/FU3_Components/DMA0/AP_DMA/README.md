# FU3 AP_DMA

SystemC 2.3.4 / TLM-2.0 DMA component for memory-to-memory, memory-to-peripheral,
and peripheral-to-memory transfers. The model provides eight channels, a 32-byte
FIFO per channel, command chaining, endian conversion, and one shared interrupt.

## Documentation

| Guide | Contents |
|---|---|
| [Usage and integration](docs/usage.md) | Port connections, MMIO helpers, transfer examples, peripheral handshake, chaining, timers, and error recovery |
| [Register reference](docs/registers.md) | Channel and global registers, field definitions, reset values, and interrupt sources |

## Build

Use Linux or WSL with CMake 3.16 or later, a C++17 compiler, and SystemC 2.3.4
built with a compatible compiler and C++ ABI. Run these commands from the
repository root, replacing the SystemC prefix with your installation path:

```sh
cmake -S components/FU3_Components/DMA0/AP_DMA -B build/fu3-ap-dma \
  -DSYSTEMC_HOME=/opt/systemc-2.3.4 \
  -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build/fu3-ap-dma --parallel 4
ctest --test-dir build/fu3-ap-dma --output-on-failure
```

If headers and libraries are installed in different locations, set
`SYSTEMC_INCLUDE_DIR` and `SYSTEMC_LIBRARY` explicitly. Set `BUILD_TESTING=OFF`
when only the library is needed.

To link the component into another CMake project, add its directory and link
your executable to `cdc::components::fu3_ap_dma`:

```cmake
set(SYSTEMC_HOME "/opt/systemc-2.3.4" CACHE PATH "SystemC installation")
add_subdirectory(
  "${CDC_VP_ROOT}/components/FU3_Components/DMA0/AP_DMA"
  "${CMAKE_CURRENT_BINARY_DIR}/fu3_ap_dma")
target_link_libraries(my_vp PRIVATE cdc::components::fu3_ap_dma)
```

## Run tests

The helper script configures, builds, and runs the suite, writing JUnit results
to the build directory:

```sh
SYSTEMC_HOME=/opt/systemc-2.3.4 \
FU3_DMA_BUILD_DIR=/tmp/fu3-ap-dma \
FU3_DMA_JOBS=4 \
  bash components/FU3_Components/DMA0/AP_DMA/tests/run_all.sh
```

Run a specific group with CTest:

```sh
ctest --test-dir build/fu3-ap-dma -R '^fu3_ap_dma_m2m$' --output-on-failure
ctest --test-dir build/fu3-ap-dma -L engines --output-on-failure
ctest --test-dir build/fu3-ap-dma -L regression --output-on-failure
```

To check for undefined behavior, use a separate build:

```sh
cmake -S components/FU3_Components/DMA0/AP_DMA -B build/fu3-ap-dma-ubsan \
  -DSYSTEMC_HOME=/opt/systemc-2.3.4 -DBUILD_TESTING=ON \
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS=-fsanitize=undefined
cmake --build build/fu3-ap-dma-ubsan --parallel 4
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  ctest --test-dir build/fu3-ap-dma-ubsan --output-on-failure
```

For GCC coverage, configure with `-DFU3_DMA_COVERAGE=ON`, run the tests, and
process the generated coverage data with gcov or gcovr.

## Transfer limits

Each command transfers 1 to 1023 bytes. The model infers 1-, 2-, or 4-byte units
from address alignment and the available transfer size; there is no unit-select
register. Bursts are limited to 16 beats and split at 4 KiB boundaries. The core
allows at most four read and four write contexts, with one credit per peripheral
endpoint in each direction.

Use the model for functional DMA and driver integration. Bus targets must allow
concurrent `b_transport` calls. AXI pin timing, circular or double buffering, 2D
stride, WRAP bursts, exclusive accesses, and peripheral-to-peripheral transfers
are outside the supported interface.
