#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DRIVER="${HN_LIVE_DRIVER_BIN:-$ROOT/build-stress/tests/live/hyprnotes_live_driver}"
[ -x "$DRIVER" ] || { printf 'missing live driver: %s\n' "$DRIVER" >&2; exit 1; }
python3 "$ROOT/tests/live/task6_driver_checks.py" --selftest-ipc
python3 "$ROOT/tests/live/driver_protocol.py" --self-test "$DRIVER"
