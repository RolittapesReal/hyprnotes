#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf -- "$TMP"' EXIT
mkdir -p "$TMP/bin" "$TMP/scratch"
cat >"$TMP/bin/hyprctl" <<'SH'
#!/bin/sh
printf '%s\n' "$*" >>"$MOCK_LOG"
case "$*" in
  "-j workspaces") printf '%s\n' "$MOCK_WORKSPACES" ;;
  "-j activeworkspace") printf '{"id":2}\n' ;;
  "dispatch "*) [ "${MOCK_DISPATCH_FAIL:-0}" = 0 ] || { echo rejected; exit 1; }; echo ok ;;
  "-j clients") echo '[]' ;;
  *) echo '[]' ;;
esac
SH
cat >"$TMP/bin/Hyprland" <<'SH'
#!/bin/sh
echo spawned >>"$MOCK_SPAWN_LOG"
exit 0
SH
cat >"$TMP/bin/fallocate" <<'SH'
#!/bin/sh
[ "${MOCK_QUOTA_FAIL:-0}" = 0 ] || exit 1
exec /usr/bin/fallocate "$@"
SH
chmod +x "$TMP/bin/"*
export PATH="$TMP/bin:$PATH" MOCK_LOG="$TMP/host.log" MOCK_SPAWN_LOG="$TMP/spawn.log"
export HYPRLAND_INSTANCE_SIGNATURE=test-host WAYLAND_DISPLAY=wayland-test HN_NESTED_AQ='AQ_NO_MODIFIERS=1 AQ_DRM_DEVICES=/dev/dri/card1' HN_LIVE_RESERVE_BYTES=1024 HN_LIVE_RESERVE_FILES=2
export HN_NESTED_DIR="$TMP/scratch/rig" HN_NESTED_HOST_SIG=test-host

expect_no_spawn() {
  local label="$1" rc=0
  shift
  rm -f "$MOCK_SPAWN_LOG" "$MOCK_LOG"
  "$ROOT/tests/live/nested_hypr.sh" start >/dev/null 2>&1 || rc=$?
  [ "$rc" -ne 0 ] || { echo "FAIL $label: start unexpectedly succeeded" >&2; exit 1; }
  [ ! -e "$MOCK_SPAWN_LOG" ] || { echo "FAIL $label: compositor spawn attempted" >&2; exit 1; }
  if grep -E 'dispatch|keyword|hl\.config' "$MOCK_LOG" 2>/dev/null | grep -Ev 'dispatch .*hl\.dsp\.exec_cmd\(' | grep -Eq '(workspace|focus|movewindow|windowrule|hl\.config)'; then
    echo "FAIL $label: unsafe host mutation/global rule command observed" >&2; exit 1
  fi
  if grep -Eq 'dispatch.*activeworkspace' "$MOCK_LOG" 2>/dev/null; then
    echo "FAIL $label: host active-workspace dispatch observed" >&2; exit 1
  fi
  echo "PASS $label: failed before spawn (exit $rc)"
}

expect_invalid_input() {
  local label="$1" rc=0
  rm -f "$MOCK_SPAWN_LOG" "$MOCK_LOG"
  "$ROOT/tests/live/nested_hypr.sh" start >/dev/null 2>&1 || rc=$?
  [ "$rc" -ne 0 ] || { echo "FAIL $label: malformed input accepted" >&2; exit 1; }
  [ ! -e "$MOCK_LOG" ] && [ ! -e "$MOCK_SPAWN_LOG" ] || { echo "FAIL $label: host command or spawn happened" >&2; exit 1; }
  echo "PASS $label: rejected before host dispatch/spawn"
}

export MOCK_WORKSPACES='[{"id":4,"name":"4"}]' MOCK_QUOTA_FAIL=1 MOCK_DISPATCH_FAIL=0
expect_no_spawn quota_reservation_failure
export MOCK_QUOTA_FAIL=0

# Config boundaries reject shell/Lua metacharacters and controls before any host call.
outside_sentinel="$TMP/sentinel-must-not-exist"
rm -f "$outside_sentinel"
export HN_NESTED_DIR="$TMP/scratch/rig;touch$outside_sentinel"
expect_invalid_input path_shell_injection
[ ! -e "$outside_sentinel" ] || { echo 'FAIL path injection created outside sentinel' >&2; exit 1; }
export HN_NESTED_DIR="$TMP/scratch/rig\\quote"
expect_invalid_input path_backslash
export HN_NESTED_DIR="$TMP/scratch/rig\"quote"
expect_invalid_input path_quote
export HN_NESTED_DIR="$TMP/scratch/rig"$'\n'"newline"
expect_invalid_input path_newline
export HN_NESTED_DIR="$TMP/scratch/rig" HN_NESTED_AQ='AQ_NO_MODIFIERS=1;touch /tmp/hn-sentinel'
expect_invalid_input AQ_shell_injection
export HN_NESTED_AQ=$'AQ_NO_MODIFIERS=1\nBAD=1'
expect_invalid_input AQ_newline
export HN_NESTED_AQ='AQ_NO_MODIFIERS=1 AQ_DRM_DEVICES=/dev/dri/card1'
export HN_NESTED_WS='4;dispatch workspace 2'
expect_invalid_input workspace_injection
unset HN_NESTED_WS
export HN_NESTED_SIZE=$'1280x720@60\n;touch /tmp/hn-size-sentinel'
expect_invalid_input Lua_control_injection
unset HN_NESTED_SIZE

export HN_NESTED_DIR="$TMP/scratch/config-failure"
mkdir -m 700 -p "$HN_NESTED_DIR"
mkdir "$HN_NESTED_DIR/hyprland.lua.tmp"
expect_no_spawn config_path_write_failure
rm -rf -- "$HN_NESTED_DIR/hyprland.lua.tmp"
chmod 700 "$HN_NESTED_DIR"
export HN_NESTED_DIR="$TMP/scratch/rig"
export HN_NESTED_DIR="$TMP/scratch/rig" MOCK_DISPATCH_FAIL=1
expect_no_spawn placement_dispatch_failure
export MOCK_DISPATCH_FAIL=0 MOCK_WORKSPACES='[{"id":2,"name":"2"}]'
expect_no_spawn missing_workspace4
export MOCK_WORKSPACES='[{"id":4,"name":"4"}]' HN_NESTED_RULE_LAUNCH=0
expect_no_spawn disabled_placement_no_direct_fallback

# Existing scratch/ledger symlinks and permissive directories fail closed.
export HN_NESTED_RULE_LAUNCH=1 HN_NESTED_DIR="$TMP/scratch/ledger-link"
mkdir -m 700 "$HN_NESTED_DIR"
ln -s "$TMP/foreign-ledger" "$HN_NESTED_DIR/pids"
expect_no_spawn symlink_ledger_rejected
rm -rf -- "$HN_NESTED_DIR"
export HN_NESTED_DIR="$TMP/scratch/token-link"
mkdir -m 700 "$HN_NESTED_DIR"
ln -s "$TMP/foreign-token" "$HN_NESTED_DIR/owner-token"
expect_no_spawn symlink_owner_token_rejected
rm -rf -- "$HN_NESTED_DIR"
export HN_NESTED_DIR="$TMP/scratch/permissive-dir"
mkdir -m 755 "$HN_NESTED_DIR"
expect_no_spawn permissive_scratch_rejected
rm -rf -- "$HN_NESTED_DIR"

# Cleanup ownership regression: a foreign PID in the ledger must never receive a signal.
foreign_pid="$$"
export HN_NESTED_DIR="$TMP/scratch/foreign-pid"
mkdir -m 700 -p "$HN_NESTED_DIR"
umask 077
printf '%s\n' "$foreign_pid" >"$HN_NESTED_DIR/pids"
chmod 600 "$HN_NESTED_DIR/pids"
stop_status=0
"$ROOT/tests/live/nested_hypr.sh" stop >/dev/null 2>&1 || stop_status=$?
[ "$stop_status" -eq 2 ] || { echo "FAIL foreign PID cleanup: expected fail-closed status 2, got $stop_status" >&2; exit 1; }
kill -0 "$foreign_pid"
echo "PASS foreign PID cleanup: refusal retained ledger and current test process remains alive"

python3 "$ROOT/tests/live/float_matrix_gate_selftest.py"
TMPDIR="$TMP" python3 "$ROOT/tests/live/process_guard_selftest.py"
