#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Run a VP_FX1_V3.0 package inside a clean AlmaLinux 9 container, the way the
# SDK README tells Windows/macOS users to (docker run --platform linux/amd64
# almalinux:9). Manual check, not part of ctest: it needs Docker and the image.
#
#   platforms/fx1_soc/package/run_docker_check.sh PACKAGE_ROOT [--toolchain DIR] [--image IMG]
#
# In the container, with nothing installed: run_vp.sh version and smoke. Then
# python3 is installed if the image lacks it (dnf, needs network) and every ISP
# sample runs. With --toolchain (a riscv-none-elf install, mounted read-only),
# the firmware is also rebuilt in the container and must equal the packaged ELF
# (GNU make comes from the host when the image has none).
# Then the console flows the README documents, in a named container: the VP in
# `run_vp.sh interactive` and the client through `docker exec` (the console
# listens on the container's 127.0.0.1 only, so a published port cannot reach
# it), and the one-terminal `run_vp.sh isp d65 interactive` flow. The image
# has neither socat nor nc, so this is the client's bash /dev/tcp path.
# The package is copied first; the original is never written.
set -euo pipefail
export PATH="/usr/bin:/bin:${PATH}"
pkg="${1:?usage: run_docker_check.sh PACKAGE_ROOT [--toolchain DIR] [--image IMG]}"
shift
image="almalinux:9" toolchain=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --toolchain) toolchain="$(cd "$2" && pwd)"; shift 2 ;;
        --image) image="$2"; shift 2 ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
done
work="$(mktemp -d "${TMPDIR:-/tmp}/fx1_docker.XXXXXX")"
name="fx1-vp-check-$$"
cleanup() {
    docker rm -f "${name}" > /dev/null 2>&1 || true
    rm -rf "${work}"
}
trap cleanup EXIT
cp -a "${pkg}" "${work}/sdk"
mounts=(-v "${work}/sdk:/workspace" -w /workspace)
# With --toolchain, GNU make is taken from the host when the image has none:
# AlmaLinux 9 host and image share glibc, and dnf may have no network.
if [[ -n "${toolchain}" ]]; then
    mounts+=(-v "${toolchain}:/toolchain:ro" -v "$(command -v make):/opt/host/make:ro")
fi
# The container runs as root (dnf needs it); the files it writes are handed
# back to the caller before it exits, so the temporary copy can be removed.
docker run --rm --platform linux/amd64 -e HOST_IDS="$(id -u):$(id -g)" "${mounts[@]}" "${image}" bash -c '
set -uo pipefail
trap "chown -R \"\${HOST_IDS}\" /workspace" EXIT
t=sw/bootloader/test/VP_FX1_V3.0
fail=0
echo "container: $(. /etc/os-release; echo "${PRETTY_NAME}"), $(uname -m)"
"${t}/run_vp.sh" version | grep -E "^fx1_soc|build type" || fail=1
"${t}/run_vp.sh" smoke | tail -n 1 || fail=1
if ! command -v python3 > /dev/null; then
    echo "python3 not in the image: dnf install -y python3"
    dnf install -y -q python3 > /dev/null 2>&1 || { echo "dnf failed (network?)"; exit 1; }
fi
for d in ${t}/isp/*/; do
    "${t}/run_vp.sh" isp "$(basename "${d}")" | tail -n 1 || fail=1
done
if [[ -d /toolchain ]]; then
    want="$(sha256sum sw/bootloader/build/VP_FX1_V3.0/freertos_cli.elf | cut -d" " -f1)"
    command -v make > /dev/null || export PATH="${PATH}:/opt/host"
    if make -s -C sw/bootloader/sources/VP_FX1_V3.0 TOOLDIR=/toolchain/bin clean all > /tmp/make.log 2>&1; then
        got="$(sha256sum sw/bootloader/build/VP_FX1_V3.0/freertos_cli.elf | cut -d" " -f1)"
        [[ "${got}" == "${want}" ]] && echo "firmware rebuilt in the container: identical to the packaged ELF" \
            || { echo "firmware rebuilt in the container differs"; fail=1; }
        "${t}/run_vp.sh" smoke | tail -n 1 || fail=1
    else
        cat /tmp/make.log; fail=1
    fi
fi
[[ ${fail} -eq 0 ]] && echo "docker check: PASS" || echo "docker check: FAIL"
exit ${fail}
'
rc=$?
[[ ${rc} -eq 0 ]] || exit ${rc}

echo "console flows in a named container (${name})"
fail=0
docker run -d --rm --platform linux/amd64 --name "${name}" -e HOST_IDS="$(id -u):$(id -g)" \
    -v "${work}/sdk:/workspace" -w /workspace "${image}" sleep 900 > /dev/null
t=/workspace/sw/bootloader/test/VP_FX1_V3.0
docker exec -d "${name}" bash -c "${t}/run_vp.sh interactive 5000 > /tmp/vp.log 2>&1"
sleep 2
(sleep 3; printf 'help\r'; sleep 2; printf 'poweroff\r'; sleep 6) \
    | timeout 60 docker exec -i "${name}" "${t}/fx1_console.sh" 5000 > "${work}/console.log" 2>&1
if grep -q "this list" "${work}/console.log" && grep -q "\[h0\] PASS" "${work}/console.log"; then
    echo "docker exec console: help, poweroff, PASS (client: bash /dev/tcp)"
else
    echo "docker exec console FAILED:"; tr -d '\r' < "${work}/console.log" | tail -n 10; fail=1
fi
(sleep 4; printf 'isp run\r'; sleep 12; printf 'poweroff\r'; sleep 25) \
    | timeout 120 docker exec -i "${name}" script -q -e -c "${t}/run_vp.sh isp d65 interactive 5001" /dev/null \
        > "${work}/isp_interactive.log" 2>&1
if tr -d '\r' < "${work}/isp_interactive.log" | grep -q "ISP PASS"; then
    echo "one-terminal isp d65 interactive: ISP PASS"
else
    echo "one-terminal ISP flow FAILED:"; tr -d '\r' < "${work}/isp_interactive.log" | tail -n 10; fail=1
fi
docker exec "${name}" bash -c 'chown -R "${HOST_IDS}" /workspace' || true
[[ ${fail} -eq 0 ]] && echo "docker console check: PASS" || echo "docker console check: FAIL"
exit ${fail}
