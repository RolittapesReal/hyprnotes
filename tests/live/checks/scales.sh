#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/common.sh"
HN_NESTED_SIZE=2560x1600@60
export HN_NESTED_SIZE
hn_live_begin 1
python3 "$HN_LIVE_DIR/task6_driver_checks.py" scales "$HN_LIVE_RIG" "$HN_LIVE_DRIVER_BIN" "$HN_NESTED_DIR"
