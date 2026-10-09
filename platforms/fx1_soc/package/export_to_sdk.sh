#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Copy a checked VP_FX1_V3.0 package into the SDK repository (the `fx1` tree:
# vp/{bin,doc}/<ver>, sw/bootloader/{sources,build,test}/<ver>). Never commits.
#
#   platforms/fx1_soc/package/export_to_sdk.sh PACKAGE_ROOT SDK_REPO [--replace] [--allow-dirty]
#
# Refuses, unless told otherwise:
#   - a package that fails check_package.sh;
#   - a package built from a dirty CDC-VP tree or whose binary revision is not
#     the packaged one (--allow-dirty: for a preview only, never a release);
#   - an SDK repository with uncommitted changes in the VP_FX1_V3.0 paths;
#   - overwriting an existing VP_FX1_V3.0 (--replace).
# The SDK's root README.md and vp/VERSION are not edited here.
set -euo pipefail
export PATH="/usr/bin:/bin:${PATH}"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
version="VP_FX1_V3.0"
pkg="${1:?usage: export_to_sdk.sh PACKAGE_ROOT SDK_REPO [--replace] [--allow-dirty]}"
sdk="${2:?usage: export_to_sdk.sh PACKAGE_ROOT SDK_REPO [--replace] [--allow-dirty]}"
shift 2
replace=0 allow_dirty=0
for o in "$@"; do
    case "${o}" in
        --replace) replace=1 ;;
        --allow-dirty) allow_dirty=1 ;;
        *) echo "export_to_sdk: unknown option ${o}" >&2; exit 2 ;;
    esac
done
die() { echo "export_to_sdk: $*" >&2; exit 1; }
pkg="$(cd "${pkg}" && pwd)"
sdk="$(cd "${sdk}" && pwd)"
git -C "${sdk}" rev-parse --git-dir > /dev/null 2>&1 || die "${sdk} is not a git repository"

"${here}/check_package.sh" "${pkg}" > /dev/null || die "check_package.sh fails on ${pkg}; run it for details"
read -r dirty matches revision < <(python3 - "${pkg}/vp/doc/${version}/BUILD_MANIFEST.json" <<'EOF'
import json, sys
s = json.load(open(sys.argv[1]))["source"]
print(str(s["dirty_at_package_time"]).lower(), str(s["revision_matches_binary"]).lower(), s["package_revision"])
EOF
)
if [[ "${dirty}" != false || "${matches}" != true ]]; then
    [[ ${allow_dirty} -eq 1 ]] || die "package from a dirty tree or a stale binary (dirty=${dirty}, \
revision matches binary=${matches}); commit CDC-VP, rebuild and repackage (--allow-dirty for a preview)"
    echo "export_to_sdk: WARNING exporting a preview from a dirty CDC-VP tree (${revision:0:12})"
fi

paths=("vp/bin/${version}" "vp/doc/${version}" "sw/bootloader/sources/${version}"
       "sw/bootloader/build/${version}" "sw/bootloader/test/${version}")
changes="$(git -C "${sdk}" status --porcelain -- "${paths[@]}")"
[[ -z "${changes}" ]] || die "uncommitted changes in the SDK's ${version} paths:
${changes}"
for p in "${paths[@]}"; do
    if [[ -e "${sdk}/${p}" ]]; then
        [[ ${replace} -eq 1 ]] || die "${sdk}/${p} exists (--replace to overwrite it)"
        rm -rf "${sdk:?}/${p}"
    fi
done
for p in "${paths[@]}"; do
    mkdir -p "$(dirname "${sdk}/${p}")"
    cp -a "${pkg}/${p}" "${sdk}/${p}"
done
(cd "${sdk}" && sha256sum --quiet -c "vp/doc/${version}/SHA256SUMS") || die "SHA256SUMS fails in ${sdk} after the copy"
"${here}/check_package.sh" "${sdk}" > /dev/null || die "check_package.sh fails in ${sdk} after the copy"
echo "export_to_sdk: ${version} (CDC-VP ${revision:0:12}) copied into ${sdk}; nothing committed."
echo "Tracked changes to review and commit:"
git -C "${sdk}" status --short -- "${paths[@]}" | sed 's/^/  /'
