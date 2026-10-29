#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build DOA2U NX (Nintendo Switch .nro) from the program ./build_game.sh
# already lifted and authenticated. Run from WSL/Linux with devkitPro:
#
#   ./switch/build_switch.sh            # Release build, 2 jobs
#   JOBS=3 ./switch/build_switch.sh     # more jobs if WSL has the RAM (~3 GB each)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
: "${DEVKITPRO:=/opt/devkitpro}"
JOBS="${JOBS:-2}"
PRESENTER="${PRESENTER:-probe}"

if [[ ! -f "${DEVKITPRO}/cmake/Switch.cmake" ]]; then
    echo "devkitPro not found at ${DEVKITPRO} (install switch-dev: sudo dkp-pacman -S switch-dev)" >&2
    exit 1
fi

# The program build_game.py lifted and authenticated, with its receipt.
GENERATED="${ROOT}/private/program/generated"
RECEIPT="${ROOT}/private/program/receipt.json"
if [[ ! -f "${RECEIPT}" || ! -f "${GENERATED}/recomp_dispatch.c" ]]; then
    echo "No lifted program in private/program. Run ./build_game.sh <disc-2.iso> first." >&2
    exit 1
fi

read_receipt() {
    python3 - "${RECEIPT}" "$1" <<'PY'
import json, sys
receipt = json.load(open(sys.argv[1], encoding="utf-8"))
value = receipt
for key in sys.argv[2].split("."):
    value = value[key]
print(value)
PY
}
MANIFEST="$(read_receipt manifest_sha256)"
XBE_SHA256="$(read_receipt recipe.xbe_sha256)"
EBP="$(read_receipt ebp_count)"
if [[ "${EBP}" != "0" ]]; then
    echo "The receipt records ${EBP} uninitialized EBP prologues; rebuild with ./build_game.sh." >&2
    exit 1
fi

echo "Program:  ${GENERATED}"
echo "Manifest: ${MANIFEST}"
echo "XBE:      ${XBE_SHA256}"

# The homebrew menu icon (256x256 JPEG) is optional. It is cover art, so it
# stays out of git: keep it in private/icon.jpg, which git never touches
# (switch/icon.jpg also works).
ICON_ARGS=()
for candidate in "${ROOT}/switch/icon.jpg" "${ROOT}/private/icon.jpg"; do
    if [[ -f "${candidate}" ]]; then
        ICON_ARGS=(-DDOA2U_NX_ICON="${candidate}")
        echo "Icon:     ${candidate}"
        break
    fi
done
if [[ ${#ICON_ARGS[@]} -eq 0 ]]; then
    echo "Icon:     none (copy a 256x256 JPEG to private/icon.jpg for one)"
fi

GENERATOR=()
if command -v ninja >/dev/null 2>&1; then GENERATOR=(-G Ninja); fi

cmake -S "${ROOT}/switch" -B "${ROOT}/build/switch" "${GENERATOR[@]}" \
    -DCMAKE_TOOLCHAIN_FILE="${DEVKITPRO}/cmake/Switch.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DRECOMP_PROGRAM_DIR="${GENERATED}" \
    -DRECOMP_PROGRAM_MANIFEST_SHA256="${MANIFEST}" \
    -DRECOMP_XBE_SHA256="${XBE_SHA256}" \
    -DDOA2U_NX_PRESENTER="${PRESENTER}" \
    "${ICON_ARGS[@]}"
cmake --build "${ROOT}/build/switch" --parallel "${JOBS}" --target doa2u-nx_nro

OUT="${ROOT}/build/switch/doa2u-nx.nro"
ls -lh "${OUT}"
echo
echo "Copy to the SD card:"
echo "  ${OUT}  ->  sdmc:/switch/doa2u-nx/doa2u-nx.nro"
echo "  extracted Disc 2 (USA)  ->  sdmc:/switch/doa2u-nx/disc/"
echo "  (private/imported-disc/disc holds it after ./build_game.sh)"
