#!/usr/bin/env bash
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT/tests/live/preflight.sh"
unset HN_LIVE_PREFLIGHT_RESERVATION HN_LIVE_PREFLIGHT_PROBE
CHECK_DIR="$ROOT/tests/live/checks"
CHECKS=(rules_applied float_fallback sticky_lifecycle focus_and_stacking workspaces multi_monitor scales tray_waybar binds ipc_loss ipc_garbage event_storm reload_and_monitor_change)
host_ws() {
    HYPRLAND_INSTANCE_SIGNATURE="${HYPRLAND_INSTANCE_SIGNATURE:-}" hyprctl -j activeworkspace 2>/dev/null |
        python3 -c 'import json,sys; print(json.load(sys.stdin)["id"])'
}
before="$(host_ws)" || { printf 'FAIL matrix: cannot read host active workspace\n'; exit 1; }
HN_NESTED_HOST_SIG="${HYPRLAND_INSTANCE_SIGNATURE:-}"
hn_preflight_workspace4 "$HN_NESTED_HOST_SIG" 4 || exit 2
hn_preflight_scratch "${TMPDIR:-/tmp}" || exit 2
failed=0
HN_LIVE_EVIDENCE_DIR="${HN_LIVE_EVIDENCE_DIR:-}"
HN_LIVE_SESSION_DIR=""
HN_LIVE_INVOCATION_OWNER_PID="$BASHPID"
HN_MATRIX_INTERRUPTED=0
hn_matrix_finish() {
    local rc=0
    [ -n "$HN_LIVE_SESSION_DIR" ] || return 2
    python3 "$ROOT/tests/live/lifecycle.py" finish "$HN_LIVE_SESSION_DIR" >/dev/null 2>&1 || rc=2
    python3 "$ROOT/tests/live/lifecycle.py" verify-clean "$HN_LIVE_SESSION_DIR" >/dev/null 2>&1 || rc=2
    return "$rc"
}
hn_matrix_signal() {
    HN_MATRIX_INTERRUPTED=1
    trap '' INT TERM
    hn_matrix_finish || :
    hn_preflight_release
    exit 2
}
trap 'hn_matrix_signal' INT TERM
trap 'hn_preflight_release' EXIT
if [ -z "$HN_LIVE_EVIDENCE_DIR" ]; then
    HN_LIVE_EVIDENCE_DIR="$(mktemp -d /tmp/opencode/hyprnotes-live-${UID}.XXXXXX)" || exit 2
    chmod 700 "$HN_LIVE_EVIDENCE_DIR" || exit 2
fi
export HN_LIVE_EVIDENCE_DIR HN_LIVE_INVOCATION_OWNER_PID

hn_matrix_float_case() {
    local setting="$1" label task_dir check_status gate_status after
    label="$setting"
    task_dir="$(mktemp -d "${TMPDIR:-/tmp}/hyprnotes-task6-${UID}-float_fallback-${label}.XXXXXX")" || return 2
    if ! { printf 'Task 6 check float_fallback case %s\n' "$setting" >"$task_dir/header" && touch "$task_dir/marker"; }; then
        printf 'INCONCLUSIVE: float_fallback case scratch setup failed before GUI startup\n' >&2
        return 2
    fi
    HN_LIVE_SESSION_DIR="$(python3 "$ROOT/tests/live/lifecycle.py" init "$HN_LIVE_EVIDENCE_DIR" "$task_dir" "$HN_LIVE_INVOCATION_OWNER_PID")" || {
        printf 'INCONCLUSIVE: cannot initialize float_fallback case session\n' >&2
        return 2
    }
    export HN_LIVE_SESSION_DIR
    printf 'RUN  float_fallback --case %s\n' "$setting"
    check_status=0
    HN_TASK6_DIR="$task_dir" "$CHECK_DIR/float_fallback.sh" --case "$setting" || check_status=$?
    gate_status=0
    hn_matrix_finish || gate_status=2
    HN_LIVE_SESSION_DIR=""
    if [ "$gate_status" -ne 0 ] || [ "$HN_MATRIX_INTERRUPTED" = 1 ]; then
        printf 'INCONCLUSIVE float_fallback case %s: controller finalization or receipt verification failed\n' "$setting"
        return 2
    fi
    after="$(host_ws)" || after=unreadable
    if [ "$after" != "$before" ]; then
        printf 'INCONCLUSIVE float_fallback case %s: host workspace changed before=%s after=%s\n' "$setting" "$before" "$after"
        return 2
    fi
    printf 'HOST_WORKSPACE float_fallback case=%s before=%s after=%s\n' "$setting" "$before" "$after"
    return "$check_status"
}

requested=("$@")
if [ "${#requested[@]}" -eq 0 ]; then requested=("${CHECKS[@]}"); fi
for name in "${requested[@]}"; do
    found=0
    for known in "${CHECKS[@]}"; do [ "$name" = "$known" ] && found=1; done
    if [ "$found" != 1 ] || [ ! -x "$CHECK_DIR/$name.sh" ]; then
        printf 'FAIL %s: missing or unknown check (not skipped)\n' "$name"
        failed=1
        continue
    fi
    if [ "$name" = float_fallback ]; then
        printf 'RUN  float_fallback\n'
        float_status=0
        for setting in false true; do
            case_status=0
            hn_matrix_float_case "$setting" || case_status=$?
            if [ "$case_status" -eq 2 ]; then
                printf 'INCONCLUSIVE float_fallback: case %s unsafe; stopping before next case\n' "$setting"
                exit 2
            elif [ "$case_status" -ne 0 ]; then
                float_status=1
            fi
        done
        if [ "$float_status" -eq 0 ]; then
            printf 'PASS float_fallback\n'
        else
            printf 'FAIL float_fallback\n'
            failed=1
        fi
        continue
    fi
    task_dir="$(mktemp -d "${TMPDIR:-/tmp}/hyprnotes-task6-${UID}-${name}.XXXXXX")" || { printf 'INCONCLUSIVE: cannot create per-check scratch directory\n' >&2; exit 2; }
    if ! { printf 'Task 6 check %s\n' "$name" >"$task_dir/header" && touch "$task_dir/marker"; }; then
        printf 'INCONCLUSIVE: per-check scratch header/touch failed before GUI startup\n' >&2; exit 2
    fi
    HN_LIVE_SESSION_DIR="$(python3 "$ROOT/tests/live/lifecycle.py" init "$HN_LIVE_EVIDENCE_DIR" "$task_dir" "$HN_LIVE_INVOCATION_OWNER_PID")" || { printf 'INCONCLUSIVE: cannot initialize durable session\n' >&2; exit 2; }
    export HN_LIVE_SESSION_DIR
    printf 'RUN  %s\n' "$name"
    check_status=0
    HN_TASK6_DIR="$task_dir" "$CHECK_DIR/$name.sh" || check_status=$?
    gate_status=0
    hn_matrix_finish || gate_status=2
    HN_LIVE_SESSION_DIR=""
    if [ "$gate_status" -ne 0 ] || [ "$HN_MATRIX_INTERRUPTED" = 1 ]; then
        printf 'INCONCLUSIVE %s: controller finalization or receipt verification failed\n' "$name"
        exit 2
    fi
    if [ "$check_status" -eq 0 ]; then
        printf 'PASS %s\n' "$name"
    elif [ "$check_status" -eq 2 ]; then
        printf 'INCONCLUSIVE %s: check declined unsafe continuation\n' "$name"
        failed=1
    else
        printf 'FAIL %s\n' "$name"
        failed=1
    fi
    after="$(host_ws)" || after=unreadable
    if [ "$after" != "$before" ]; then
        printf 'INCONCLUSIVE %s: host workspace changed before=%s after=%s; stopping live checks\n' "$name" "$before" "$after"
        exit 2
    else
        printf 'HOST_WORKSPACE %s before=%s after=%s\n' "$name" "$before" "$after"
    fi
    if [ "$check_status" -eq 2 ]; then
        exit 2
    fi
done

[ "$failed" -eq 0 ]
