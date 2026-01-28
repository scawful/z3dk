#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${Z3DK_BUILD_DIR:-${ROOT}/build}"
CMAKE_ARGS=()

if [[ -n "${Z3DK_CS_TEST:-}" ]]; then
  CMAKE_ARGS+=("-DASAR_GEN_CS_TEST=${Z3DK_CS_TEST}")
elif [[ "$(uname)" == "Darwin" ]]; then
  # Mono is commonly x86_64 on macOS; skip C# tests unless explicitly enabled.
  CMAKE_ARGS+=("-DASAR_GEN_CS_TEST=OFF")
fi

# Pre-flight check
if [[ ! -f "${ROOT}/dummy_rom.sfc" ]]; then
    echo "Error: dummy_rom.sfc not found in ${ROOT}"
    echo "Run 'git restore dummy_rom.sfc' or recreate it."
    exit 1
fi

mkdir -p "${BUILD_DIR}"
cmake -S "${ROOT}" -B "${BUILD_DIR}" "${CMAKE_ARGS[@]}"
cmake --build "${BUILD_DIR}" --target run-tests
