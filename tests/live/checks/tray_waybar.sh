#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/common.sh"
hn_live_begin 0
dbus-run-session -- python3 "$HN_LIVE_DIR/task6_driver_checks.py" tray "$HN_LIVE_RIG" "$HN_LIVE_DRIVER_BIN" "$HN_NESTED_DIR"
