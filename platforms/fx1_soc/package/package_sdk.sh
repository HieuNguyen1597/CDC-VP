#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Assemble the VP_FX1_V3.0 SDK delivery (FX1 SoC VP + FreeRTOS CLI firmware)
# from a CDC-VP build that has target fx1_all built.
#
#   platforms/fx1_soc/package/package_sdk.sh --build-dir DIR --out DIR
#       [--tooldir DIR] [--sample NAME=RAW[,profile=FILE][,internal]] ... [--allow-dev-build]
#
# --out receives the tree in the SDK repository layout (vp/..., sw/...), so an
# export is a copy (export_to_sdk.sh). Nothing outside --out is written.
#
# What goes in, and where it comes from:
#   vp/bin/VP_FX1_V3.0       fx1_soc (RPATH $ORIGIN) + the libsystemc it needs
#   vp/doc/VP_FX1_V3.0       README, memory map, licences, BUILD_MANIFEST.json,
#                            SHA256SUMS (tracked files only, see below)
#   sw/bootloader/sources/   the FreeRTOS CLI sources, BSP, ISP driver and the
#                            FreeRTOS-Kernel subset it compiles, with a
#                            stand-alone Makefile; copied, never rewritten
#   sw/bootloader/build/     outputs of that Makefile, run here by this script
#                            (the SDK repository does not track build/)
#   sw/bootloader/test/      run_vp.sh, console client, replay scripts, the ISP
#                            samples (job files + golden NV12 SHA-256 from the
#                            stand-alone ISP model)
#
# Default sample: d65, components/isp_tlm/input/D65_raw_2688x1520_5376.raw
# (cleared, ASSET_PROVENANCE.md) with a 'full' test profile from make_profile.py.
#
# Exit status: 0 the tree is complete and self-consistent, 1 otherwise.
set -euo pipefail
export PATH="/usr/bin:/bin:${PATH}"

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
src="$(cd "${here}/../../.." && pwd)"
version="VP_FX1_V3.0"
build_dir="" out="" tooldir="" allow_dev=0
samples=()

die() { echo "package_sdk: $*" >&2; exit 1; }
log() { echo "[package] $*"; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir) build_dir="$(cd "$2" && pwd)"; shift 2 ;;
        --out) out="$2"; shift 2 ;;
        --tooldir) tooldir="$2"; shift 2 ;;
        --sample) samples+=("$2"); shift 2 ;;
        --allow-dev-build) allow_dev=1; shift ;;
        -h|--help) sed -n '3,30p' "$0"; exit 0 ;;
        *) die "unknown option $1" ;;
    esac
done
[[ -n "${build_dir}" && -n "${out}" ]] || die "usage: package_sdk.sh --build-dir DIR --out DIR (see --help)"
[[ ${#samples[@]} -gt 0 ]] || samples=("d65=${src}/components/isp_tlm/input/D65_raw_2688x1520_5376.raw")

sim="${build_dir}/platforms/fx1_soc/fx1_soc"
ref_tool="${build_dir}/components/FX1_Components/isp/fx1_isp_run_raw"
isp="${src}/components/FX1_Components/isp"
fw="${src}/fw/fx1_soc"
kernel="${src}/third_party/FreeRTOS-Kernel"
vpp="${src}/third_party/riscv-vp-plusplus"
for f in "${sim}" "${ref_tool}"; do
    [[ -x "${f}" ]] || die "missing ${f}: build target fx1_all in ${build_dir} first"
done
if [[ -z "${tooldir}" ]]; then
    prefix="$(sed -n 's/^FX1_RISCV_PREFIX:[A-Z]*=//p' "${build_dir}/CMakeCache.txt")"
    [[ -n "${prefix}" ]] || die "no FX1_RISCV_PREFIX in ${build_dir}/CMakeCache.txt; pass --tooldir"
    tooldir="$(dirname "${prefix}")"
fi
[[ -x "${tooldir}/riscv-none-elf-gcc" ]] || die "no riscv-none-elf-gcc in ${tooldir}"
grep -q '^#define tskKERNEL_VERSION_NUMBER *"V11.2.0"' "${kernel}/include/task.h" \
    || die "FreeRTOS-Kernel V11.2.0 not found in ${kernel} (tools/third_party/setup_third_party.sh)"
build_type="$("${sim}" --version 2>/dev/null | sed -n 's/^  build type *\([^,]*\),.*/\1/p')"
if [[ "${build_type}" != "Release" && ${allow_dev} -eq 0 ]]; then
    die "the simulator is a '${build_type}' build; package a Release build (or pass --allow-dev-build)"
fi

rm -rf "${out}"
mkdir -p "${out}"
out="$(cd "${out}" && pwd)"
bin="${out}/vp/bin/${version}"
doc="${out}/vp/doc/${version}"
sdk="${out}/sw/bootloader/sources/${version}"
fwout="${out}/sw/bootloader/build/${version}"
test="${out}/sw/bootloader/test/${version}"
mkdir -p "${bin}" "${doc}/licenses" "${sdk}" "${fwout}" "${test}/isp"

# ── the VP ────────────────────────────────────────────────────────────────────
log "VP binary and runtime"
cp "${sim}" "${bin}/fx1_soc"
rpath="$(readelf -d "${bin}/fx1_soc" | sed -n 's/.*(R[UN]*PATH).*\[\(.*\)\]/\1/p')"
[[ "${rpath}" == '$ORIGIN' ]] || die "fx1_soc RPATH is '${rpath}', expected \$ORIGIN (cdc_make_portable)"
needed="$(readelf -d "${bin}/fx1_soc" | sed -n 's/.*NEEDED.*\[\(libsystemc[^]]*\)\]/\1/p')"
[[ -n "${needed}" && -f "${build_dir}/platforms/fx1_soc/${needed}" ]] || die "SystemC runtime ${needed} not beside ${sim}"
cp -L "${build_dir}/platforms/fx1_soc/${needed}" "${bin}/${needed}"

# ── firmware sources: the exact files the CDC-VP build compiles ──────────────
log "firmware sources"
mkdir -p "${sdk}/app" "${sdk}/bsp" "${sdk}/include/fx1" "${sdk}/include/dma" "${sdk}/include/fx1_isp" \
         "${sdk}/drivers/isp" "${sdk}/isp_selftest" "${sdk}/demo"
cp "${here}/sdk/Makefile" "${here}/sdk/README.md" "${sdk}/"
cp "${fw}"/freertos/*.c "${fw}"/freertos/*.h "${fw}"/freertos/*.S "${sdk}/app/"
cp "${fw}"/common/crt0.S "${fw}"/common/fx1_fw.c "${fw}"/common/fx1_fw.h "${fw}"/common/fx1.ld.S "${sdk}/bsp/"
cp "${src}/components/FX1_Components/fx1_map/include/fx1/fx1_memory_map.h" "${sdk}/include/fx1/"
cp "${src}/components/FX1_Components/dma/include/dma/fx1_dma_regs.h" "${sdk}/include/dma/"
cp "${isp}/include/fx1_isp/fx1_isp_csr.h" "${isp}/driver/include/fx1_isp/fx1_isp_drv.h" "${sdk}/include/fx1_isp/"
cp "${isp}/driver/src/fx1_isp_drv.c" "${sdk}/drivers/isp/"
cp "${fw}"/tests/isp_stream/isp_common.c "${fw}"/tests/isp_stream/isp_common.h "${fw}"/tests/isp_stream/isp_fixture.h \
   "${sdk}/isp_selftest/"
cp "${fw}/demo/isp_raw_frame/isp_demo_job.h" "${sdk}/demo/"
k="${sdk}/third_party/FreeRTOS-Kernel"
mkdir -p "${k}/include" "${k}/portable/MemMang" "${k}/portable/GCC/RISC-V"
cp "${kernel}/LICENSE.md" "${kernel}/tasks.c" "${kernel}/queue.c" "${kernel}/list.c" "${k}/"
cp "${kernel}"/include/*.h "${k}/include/"
cp "${kernel}/portable/MemMang/heap_4.c" "${k}/portable/MemMang/"
cp "${kernel}"/portable/GCC/RISC-V/{port.c,portASM.S,portContext.h,portmacro.h} "${k}/portable/GCC/RISC-V/"

# ── firmware outputs, from the shipped Makefile ──────────────────────────────
log "firmware build with the SDK Makefile (${tooldir})"
make -s -C "${sdk}" TOOLDIR="${tooldir}" OUT="${fwout}" all > "${out}/.make.log" 2>&1 \
    || { cat "${out}/.make.log" >&2; die "the SDK Makefile failed"; }
rm -f "${out}/.make.log"

# ── test launchers and ISP samples ───────────────────────────────────────────
log "launchers"
cp "${here}/test/run_vp.sh" "${here}/test/isp_run.txt" "${test}/"
cp "${fw}/freertos/tests/cli_smoke.txt" "${test}/"
cp "${src}/platforms/fx1_soc/tools/fx1_console.sh" "${test}/"
cp "${src}/platforms/fx1_soc/demo/isp_pack.py" "${test}/"
chmod +x "${test}/run_vp.sh" "${test}/fx1_console.sh" "${test}/isp_pack.py"

scratch="$(mktemp -d "${TMPDIR:-/tmp}/fx1_package.XXXXXX")"
trap 'rm -rf "${scratch}"' EXIT
sample_json="${scratch}/samples.json"
echo "[" > "${sample_json}"
first=1
for spec in "${samples[@]}"; do
    name="${spec%%=*}"
    rest="${spec#*=}"
    raw="${rest%%,*}"
    profile="" internal=0
    IFS=',' read -r -a opts <<< "${rest#"${raw}"}"
    for o in "${opts[@]}"; do
        case "${o}" in
            "") ;;
            profile=*) profile="${o#profile=}" ;;
            internal) internal=1 ;;
            *) die "sample ${name}: unknown option ${o}" ;;
        esac
    done
    [[ "${name}" =~ ^[a-z0-9_]+$ ]] || die "sample name '${name}': use [a-z0-9_]"
    [[ -f "${raw}" ]] || die "sample ${name}: RAW ${raw} not found"
    log "ISP sample ${name}: $(basename "${raw}")"
    w="${scratch}/${name}"
    mkdir -p "${w}"
    if [[ -z "${profile}" ]]; then  # a deterministic 'full' test profile computed from this frame
        python3 "${isp}/tools/raw_fixture.py" convert --in "${raw}" --format le16_raw10_msb --width 2688 \
            --height 1520 --bayer BGGR --out "${w}/frame.isp16" > "${w}/convert.log"
        python3 "${isp}/tools/make_profile.py" --input "${w}/frame.isp16" --width 2688 --height 1520 \
            --bayer BGGR --preset full --out "${w}/${name}_bggr_2688x1520_full.csrw" > "${w}/profile.log"
        profile="${w}/${name}_bggr_2688x1520_full.csrw"
    fi
    d="${test}/isp/${name}"
    mkdir -p "${d}"
    python3 "${src}/platforms/fx1_soc/demo/run_isp_demo.py" --raw "${raw}" --profile "${profile}" \
        --build-dir "${build_dir}" --out "${w}/job" --prepare-only --job-env "${d}/job.env" > "${w}/prepare.log"
    cp "${w}/job/input.isp16" "${w}/job/job.bin" "${w}/job/profile.bin" "${d}/"
    cp "${profile}" "${d}/profile.csrw"
    # Golden: the stand-alone ISP model, same input and profile (not the VP run).
    mkdir -p "${w}/ref"
    "${ref_tool}" --input "${w}/job/input.isp16" --width 2688 --height 1520 --profile "${profile}" --frames 1 \
        --out "${w}/ref/frame" > "${w}/ref.log" 2>&1 && [[ -s "${w}/ref/frame.nv12" ]] \
        || { cat "${w}/ref.log" >&2; die "sample ${name}: the reference ISP model produced no NV12"; }
    golden="$(sha256sum "${w}/ref/frame.nv12" | cut -d' ' -f1)"
    echo "${golden}" > "${d}/expected_nv12.sha256"
    raw_sha="$(sha256sum "${raw}" | cut -d' ' -f1)"
    {
        if [[ ${internal} -eq 1 ]]; then
            echo "$(basename "${raw}"), 2688x1520 BGGR 10-bit -- INTERNAL USE ONLY, not cleared for public release"
        else
            echo "$(basename "${raw}"), 2688x1520 BGGR 10-bit -- Apache-2.0 project asset (CDC-VP ASSET_PROVENANCE.md)"
        fi
        echo "RAW SHA-256       ${raw_sha}"
        echo "format            le16_raw10_msb, converted to the ISP 16-bit container (input.isp16)"
        echo "profile           profile.csrw ($(basename "${profile}"), $(grep -c -v -e '^#' -e '^\s*$' "${profile}") writes),"
        echo "                  a test profile computed from this frame, not a sensor calibration"
        echo "expected NV12     ${golden}"
        echo "                  from the stand-alone ISP model (fx1_isp_run_raw), same input and profile"
    } > "${d}/SAMPLE.txt"
    [[ ${first} -eq 1 ]] || echo "," >> "${sample_json}"
    first=0
    printf '{"name": "%s", "raw": "%s", "raw_sha256": "%s", "profile": "%s", "expected_nv12_sha256": "%s", "internal_only": %s}\n' \
        "${name}" "$(basename "${raw}")" "${raw_sha}" "$(basename "${profile}")" "${golden}" \
        "$([[ ${internal} -eq 1 ]] && echo true || echo false)" >> "${sample_json}"
done
echo "]" >> "${sample_json}"
[[ -d "${test}/isp/d65" ]] || die "the default sample d65 is missing (run_vp.sh isp uses it)"

# ── documentation and licences ───────────────────────────────────────────────
log "documentation and licences"
cp "${here}/docs/README.md" "${here}/docs/MEMORY_MAP.md" "${doc}/"
cp "${src}/LICENSE" "${doc}/licenses/Apache-2.0.txt"
cp "${src}/NOTICE" "${doc}/licenses/CDC-VP-NOTICE.txt"
cp "${src}/THIRD_PARTY.md" "${doc}/licenses/THIRD_PARTY.md"
cp "${vpp}/LICENSE" "${doc}/licenses/RISCV-VP-PLUSPLUS.MIT.txt"
cmake -DSOURCE_DIR="${src}" -DSOFTFLOAT_SOURCE="${vpp}/vp/src/vendor/softfloat/f32_add.c" \
      -DOUTPUT="${doc}/licenses/BERKELEY-SOFTFLOAT-3d.BSD-3-Clause.txt" \
      -P "${src}/platforms/riscv_vpp_compiler_vp/cmake/write_softfloat_notice.cmake" > /dev/null
systemc_home="$(sed -n 's/^SYSTEMC_HOME:[A-Z]*=//p' "${build_dir}/CMakeCache.txt")"
[[ -f "${systemc_home}/share/doc/systemc/LICENSE" ]] || die "no SystemC LICENSE under ${systemc_home}"
cp "${systemc_home}/share/doc/systemc/LICENSE" "${doc}/licenses/SYSTEMC.Apache-2.0.txt"
cp "${kernel}/LICENSE.md" "${doc}/licenses/FREERTOS-KERNEL.MIT.txt"

# ── manifest and checksums ───────────────────────────────────────────────────
log "manifest and checksums"
python3 "${here}/write_manifest.py" --out "${out}" --source "${src}" --build-dir "${build_dir}" \
    --tooldir "${tooldir}" --samples "${sample_json}" --manifest "${doc}/BUILD_MANIFEST.json"
# SHA256SUMS covers what the SDK repository tracks: not sw/bootloader/build/,
# which it ignores and every user regenerates (its hashes are in the manifest).
(cd "${out}" && find vp sw -type f ! -path "sw/bootloader/build/*" ! -name SHA256SUMS -print0 \
    | LC_ALL=C sort -z | xargs -0 sha256sum) > "${doc}/SHA256SUMS"

"${here}/check_package.sh" "${out}"
log "done: ${out}"
