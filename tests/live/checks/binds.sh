#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/common.sh"
for f in "$HN_LIVE_ROOT"/packaging/share/hyprland/hyprnotes.lua "$HN_LIVE_ROOT"/packaging/share/hyprland/hyprnotes.conf; do
    [ -f "$f" ] || { echo "missing snippet $f" >&2; exit 1; }
    Hyprland --verify-config -c "$f"
done
hn_live_begin 1
python3 "$HN_LIVE_DIR/task6_driver_checks.py" binds "$HN_LIVE_RIG" "$HN_LIVE_DRIVER_BIN" "$HN_NESTED_DIR" "$HN_LIVE_BIN" "$HN_LIVE_ROOT"
