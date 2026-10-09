# fx1_soc SDK packaging (VP_FX1_V3.0)

This directory turns a CDC-VP build of `fx1_soc` into the `VP_FX1_V3.0`
delivery for the firmware and software teams. The delivery uses the layout of
the SDK repository (`fx1`: `vp/{bin,doc}/<ver>`,
`sw/bootloader/{sources,build,test}/<ver>`), so exporting it is a copy.

| File | Role |
|---|---|
| `package_sdk.sh` | assembles the tree. Behind the CMake target `fx1_soc_package` |
| `check_package.sh` | checks a tree: contents, `RPATH=$ORIGIN` + `ldd` with an empty environment, licence texts, `SHA256SUMS`, manifest vs. files and `--version` |
| `run_packaging_regression.sh` | the distribution gate, ctest `fx1_packaging` (label `fx1_packaging`, not in `run_ci.sh`) |
| `run_docker_check.sh` | manual: runs the package in a clean `almalinux:9` container |
| `export_to_sdk.sh` | copies a checked package into the SDK repository; never commits |
| `write_manifest.py` | `BUILD_MANIFEST.json` |
| `sdk/` | the SDK firmware `Makefile` and README, shipped as is |
| `test/` | `run_vp.sh` and the `isp run` replay, shipped as is |
| `docs/` | the delivery's README and memory map |

## What goes in

| Item | Source |
|---|---|
| **VP** | `fx1_soc` from the build, made portable by `cdc_make_portable` (`RPATH=$ORIGIN`), with the `libsystemc.so.2.3` it needs |
| **Firmware sources** | the files the CDC-VP build compiles for `freertos_cli.elf`, copied, never rewritten, plus the FreeRTOS-Kernel V11.2.0 subset |
| **Firmware outputs** | built by the shipped `sdk/Makefile` during packaging. That Makefile is location-independent (`-ffile-prefix-map`), so a user's rebuild gives the packaged ELF byte for byte with the same compiler |
| **ISP samples** | `isp/<sample>/` holds the prepared job (`run_isp_demo.py --prepare-only --job-env`) and the golden NV12 SHA-256 from the stand-alone model (`fx1_isp_run_raw`), not from the VP |
| **Docs and licences** | `docs/`, `LICENSE`, `NOTICE`, `THIRD_PARTY.md`, the VP++ MIT licence, the SoftFloat notice (extracted from a compiled source), the SystemC licence, the FreeRTOS MIT licence |

**ISP samples:**

| Sample | Source | Profile | Status |
|---|---|---|---|
| `d65` (default) | `components/isp_tlm/input/D65_raw_2688x1520_5376.raw` | 'full' test profile from `make_profile.py` | cleared, `ASSET_PROVENANCE.md` |
| `outdoor1` | the RAW pointed to by `FX1_DEMO_RAW` | `outdoor1_bggr_2688x1520_full.csrw` | **internal only**: not cleared for public release |

`outdoor1` is added only when `FX1_DEMO_RAW` is set.

**`SHA256SUMS`** covers what the SDK repository tracks, so it does not cover
`build/`: that repository ignores `sw/bootloader/build/`, and every user
rebuilds it. The firmware hashes are in the manifest.

## Making a release

The official package must come from a committed, clean CDC-VP tree. The
export refuses a dirty one.

```bash
export CC=/usr/bin/gcc CXX=/usr/bin/g++ PATH=/usr/bin:/bin:$PATH
cd CDC-VP && git status --short                          # must be empty

# 1. the gate: fresh Release build, package, move, run, rebuild firmware
FX1_DEMO_RAW=<outdoor1 RAW> platforms/fx1_soc/package/run_packaging_regression.sh

# 2. the release build and package (Release, FX1 only)
cmake -S . -B build-rel -G Ninja -DCMAKE_BUILD_TYPE=Release -DCDC_BUILD_FX1_SOC=ON \
      -DCDC_BUILD_MINI_TLM=OFF -DCDC_BUILD_CPU_EVAL=OFF -DCDC_BUILD_CUSTOM_SOC=OFF -DCDC_BUILD_NOC_SOC=OFF \
      -DCDC_BUILD_TPU_V3_SOC=OFF -DCDC_BUILD_RISCV_VPP_COMPILER_VP=OFF -DFX1_DEMO_RAW=<outdoor1 RAW>
cmake --build build-rel --target fx1_soc_package -j1      # -> out/VP_FX1_V3.0

# 3. optional: a clean AlmaLinux 9 container, with the firmware rebuilt inside
platforms/fx1_soc/package/run_docker_check.sh out/VP_FX1_V3.0 --toolchain /opt/toolchains/riscv-none-elf

# 4. into the SDK repository (review and commit there by hand)
platforms/fx1_soc/package/export_to_sdk.sh out/VP_FX1_V3.0 ../fx1
```

**The gate ignores the ambient `SYSTEMC_HOME`.** A login shell may point it at
another SystemC, such as the one Fast Models ships. The gate uses
`PACKAGING_SYSTEMC_HOME`, default `/opt/systemc-2.3.4`.

`export_to_sdk.sh` leaves two files of the SDK repository for you to edit
together with the commit:
- the root `README.md`, which needs the package table row;
- `vp/VERSION`.

## Limits

- **Fixed sample geometry.** Samples are 2688×1520 BGGR `le16_raw10_msb`
  frames; `package_sdk.sh` assumes that geometry when it computes a profile.
- **ELF reproducibility needs the same compiler.** It holds for the same
  cross compiler; the manifest records which one was used.
- **The package is x86-64 Linux only.** It is validated on AlmaLinux 9 and in
  the `almalinux:9` container.
