#!/usr/bin/env bash
set -euo pipefail

# Compatibility wrapper for older docs/tools.
exec "$(cd "$(dirname "$0")" && pwd)/tests/scripts/run_tests.sh" "$@"
