#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIST="${ROOT}/dist"

mkdir -p "${DIST}"

if [ ! -d "${ROOT}/node_modules" ]; then
  (cd "${ROOT}" && npm install)
fi

VERSION="$(node -p "require('${ROOT}/package.json').version")"
OUT="${DIST}/z3dk-${VERSION}.vsix"

(cd "${ROOT}" && npx --yes @vscode/vsce package --out "${OUT}")

echo "VSIX written: ${OUT}"
