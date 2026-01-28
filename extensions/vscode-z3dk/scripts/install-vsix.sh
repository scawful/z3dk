#!/usr/bin/env bash
set -euo pipefail

CLI="${1:-code}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION="$(node -p "require('${ROOT}/package.json').version")"
VSIX="${ROOT}/dist/z3dk-${VERSION}.vsix"

if [ ! -f "${VSIX}" ]; then
  "${ROOT}/scripts/package-vsix.sh"
fi

"${CLI}" --install-extension "${VSIX}"

echo "Installed ${VSIX} via ${CLI}"
