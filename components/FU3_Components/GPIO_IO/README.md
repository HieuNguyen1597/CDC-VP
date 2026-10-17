# FU3 GPIO and IO_PAD

SystemC/TLM-2.0 model of GPIOA/B/C and digital IO_PAD function selection,
based on the FTEL GPIO HDS V1.0 and IO_PAD HDS V1.0.

## Build and test

Run from the repository root in Linux/WSL with a C++17 compiler and SystemC 2.3.4:

```sh
cmake -S components/FU3_Components/GPIO_IO -B build/fu3-gpio-full \
  -DSYSTEMC_INCLUDE_DIR=/opt/systemc-2.3.4/include \
  -DSYSTEMC_LIBRARY=/opt/systemc-2.3.4/lib/libsystemc.so \
  -DFU3_GPIO_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build/fu3-gpio-full -j4
ctest --test-dir build/fu3-gpio-full --output-on-failure
```

Link `fu3::gpio_io` to use the model. The repository build includes it when
`CDC_BUILD_FU3_SOC=ON`; another platform can use `add_subdirectory`.

## Documentation

| Document | Contents |
| --- | --- |
| [Integration](docs/usage.md) | Ports, timing, package routing and modeling boundaries |
| [Registers](docs/registers.md) | GPIO offsets, access rules, reset values and interrupt modes |
| [Testing](docs/testing.md) | Test scenarios, execution and troubleshooting |

The model exposes external function-select inputs and per-bank power events.
Separate IOSEL/WAKE system registers and physical pad electrical behavior
require additional specifications; see [modeling boundaries](docs/usage.md#modeling-boundaries).
