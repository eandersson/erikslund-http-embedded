#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

HARNESS=e2e/run.sh

if [ ! -f "$HARNESS" ]; then
    echo "==> FAIL: $HARNESS is missing, and it is the harness this script exists to run" >&2
    exit 1
fi

# Windows checkouts do not reliably preserve executable bits.
exec bash "$HARNESS" "$@"
