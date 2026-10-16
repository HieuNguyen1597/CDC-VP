#!/usr/bin/env bash
set -euo pipefail

dma_test_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
dma_source_dir="$(cd -- "${dma_test_dir}/.." && pwd)"
dma_default_prefix="${dma_source_dir}/../../../../../deps/systemc-2.3.4"
dma_build_dir="${FU3_DMA_BUILD_DIR:-/tmp/fu3-ap-dma-full}"
dma_systemc_prefix="${SYSTEMC_HOME:-${dma_default_prefix}}"
cmake -S "${dma_source_dir}" -B "${dma_build_dir}" \
  -DSYSTEMC_HOME="${dma_systemc_prefix}" \
  -DBUILD_TESTING=ON -DFU3_DMA_COVERAGE="${FU3_DMA_COVERAGE:-OFF}"
cmake --build "${dma_build_dir}" --parallel "${FU3_DMA_JOBS:-2}"
ctest --test-dir "${dma_build_dir}" --output-on-failure \
  --output-junit "${dma_build_dir}/results.xml"
