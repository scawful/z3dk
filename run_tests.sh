#!/usr/bin/env bash
set -euo pipefail

# Compatibility wrapper for older docs/tools.
exec ./tests/scripts/run_tests.sh "$@"
