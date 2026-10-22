#!/usr/bin/env bash
set -euo pipefail

HN_LIVE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HN_LIVE_ROOT="$(cd "$HN_LIVE_DIR/../.." && pwd)"
HN_LIVE_RIG="$HN_LIVE_DIR/nested_hypr.sh"
HN_LIVE_BIN="${HN_APP_BIN:-$HN_LIVE_ROOT/build-stress/src/app/hyprnotes}"
HN_LIVE_DRIVER_BIN="${HN_LIVE_DRIVER_BIN:-$HN_LIVE_ROOT/build-stress/tests/live/hyprnotes_live_driver}"
HN_TASK6_DIR="${HN_TASK6_DIR:-${TMPDIR:-/tmp}/hyprnotes-task6-${UID}-${BASHPID}}"
HN_NESTED_DIR="$HN_TASK6_DIR"
HN_NESTED_WS=4
HN_NESTED_HOST_SIG="${HYPRLAND_INSTANCE_SIGNATURE:-}"
HN_LIVE_STARTED=0
HN_LIVE_HOST_WS=""
HN_LIVE_HOST_WS=""
HN_LIVE_SESSION_DIR="${HN_LIVE_SESSION_DIR:-}"
HN_LIVE_EVIDENCE_DIR="${HN_LIVE_EVIDENCE_DIR:-}"
HN_LIVE_INVOCATION_OWNER_PID="${HN_LIVE_INVOCATION_OWNER_PID:-$BASHPID}"
HN_LIVE_FINALIZED=0
HN_LIVE_INTERRUPTED=0

hn_host_ws() {
    HYPRLAND_INSTANCE_SIGNATURE="$HN_NESTED_HOST_SIG" hyprctl -j activeworkspace |
        python3 -c 'import json,sys; print(json.load(sys.stdin)["id"])'
}
hn_host_clients() {
    HYPRLAND_INSTANCE_SIGNATURE="$HN_NESTED_HOST_SIG" hyprctl -j clients
}

hn_ctl() { "$HN_LIVE_RIG" hyprctl "$@"; }
hn_json_clients() { hn_ctl -j clients; }

hn_live_ensure_session() {
    [ -n "$HN_LIVE_SESSION_DIR" ] && return 0
    [ ! -L "$HN_TASK6_DIR" ] || return 2
    ( umask 077; mkdir -p "$HN_TASK6_DIR" ) && chmod 700 "$HN_TASK6_DIR" || return 2
    if [ -z "$HN_LIVE_EVIDENCE_DIR" ]; then
        HN_LIVE_EVIDENCE_DIR="$(mktemp -d /tmp/opencode/hyprnotes-live-${UID}.XXXXXX)" || return 2
        chmod 700 "$HN_LIVE_EVIDENCE_DIR" || return 2
    fi
    HN_LIVE_SESSION_DIR="$(python3 "$HN_LIVE_DIR/lifecycle.py" init "$HN_LIVE_EVIDENCE_DIR" "$HN_TASK6_DIR" "$HN_LIVE_INVOCATION_OWNER_PID")" || return 2
    export HN_LIVE_EVIDENCE_DIR HN_LIVE_SESSION_DIR HN_LIVE_INVOCATION_OWNER_PID HN_NESTED_DIR
}

hn_live_begin() {
    local rules="${1:-0}"
    hn_live_ensure_session || return 2
    [ -x "$HN_LIVE_BIN" ] || { printf 'missing app binary: %s\n' "$HN_LIVE_BIN" >&2; return 1; }
    [ -x "$HN_LIVE_DRIVER_BIN" ] || { printf 'missing test driver: %s (build with -DHN_LIVE_TESTS=ON or set HN_LIVE_DRIVER_BIN)\n' "$HN_LIVE_DRIVER_BIN" >&2; return 1; }
    [ -n "$HN_NESTED_HOST_SIG" ] || { printf 'host Hyprland signature unavailable\n' >&2; return 1; }
    HN_LIVE_HOST_WS="$(hn_host_ws)"
    [ -n "$HN_LIVE_HOST_WS" ] || { printf 'cannot snapshot host workspace\n' >&2; return 1; }
    export HN_NESTED_DIR HN_NESTED_WS HN_NESTED_HOST_SIG HN_NESTED_RULES="$rules"
    local start_out start_rc=0
    start_out="$("$HN_LIVE_RIG" start 2>&1)" || start_rc=$?
    HN_LIVE_STARTED=1
    if [ "$start_rc" -ne 0 ]; then
        printf 'nested start failed (%s):\n%s\n' "$start_rc" "$start_out" >&2
        return "$start_rc"
    fi
    "$HN_LIVE_RIG" env >/dev/null
    # Validate through the CLI, then source only its private, controller-bound environment file.
    # CLI validation may emit diagnostic identity data; never eval stdout as shell code.
    # shellcheck disable=SC1090
    source "$HN_NESTED_DIR/env"
    mkdir -p "$HN_NESTED_DIR/evidence"
}

hn_app() { "$HN_LIVE_RIG" run "$HN_LIVE_BIN" "$@"; }

hn_start_app() {
    mkdir -p "$HN_NESTED_DIR/app"
    hn_app --background >"$HN_NESTED_DIR/app/background.log" 2>&1 &
    HN_LIVE_APP_LAUNCH_PID=$!
    export HN_LIVE_APP_LAUNCH_PID
    # Wait for the controller to record the app identity (no fixed sleep; bounded 15 s).
    local i recorded=0
    for (( i=0; i<150; i++ )); do
        kill -0 "$HN_LIVE_APP_LAUNCH_PID" 2>/dev/null || {
            printf 'background app process exited; see %s\n' "$HN_NESTED_DIR/app/background.log" >&2
            return 1
        }
        if python3 -c 'import json,sys
st=json.load(open(sys.argv[1]+"/state.json"))
sys.exit(0 if not st.get("pending") and any(r.get("kind")=="app" for r in st.get("records",[])) else 1)' "$HN_LIVE_SESSION_DIR" 2>/dev/null; then
            recorded=1; break
        fi
        sleep 0.1
    done
    [ "$recorded" = 1 ] || { printf 'background app identity not recorded in time\n' >&2; return 1; }
    # Identity is recorded, but the app's IPC listener needs time before clients are sent (legacy grace).
    sleep 1
}

hn_wait_clients() {
    local count="$1" i actual
    for i in $(seq 1 100); do
        actual="$(hn_json_clients | python3 -c 'import json,sys; print(sum(c.get("class")=="hyprnotes" for c in json.load(sys.stdin)))')"
        [ "$actual" -ge "$count" ] && return 0
        sleep 0.1
    done
    printf 'timed out waiting for %s Hyprnotes clients\n' "$count" >&2
    hn_json_clients >&2
    return 1
}

hn_do_action() {
    local rc=0
    hn_app "$1" >"$HN_NESTED_DIR/app/action-${1#--}.log" 2>&1 || rc=$?
    if [ "$rc" -ne 0 ]; then
        printf 'app action %s exited %s:\n' "$1" "$rc" >&2
        cat "$HN_NESTED_DIR/app/action-${1#--}.log" >&2
    fi
    return "$rc"
}

hn_client_records() {
    hn_json_clients | python3 -c 'import json,sys
for c in json.load(sys.stdin):
 if c.get("class")=="hyprnotes":
  print(json.dumps({"address":c["address"],"initialTitle":c.get("initialTitle",""),"floating":c.get("floating"),"pinned":c.get("pinned"),"at":c.get("at"),"size":c.get("size"),"workspace":c.get("workspace",{}).get("id"),"monitor":c.get("monitor"),"focusHistoryID":c.get("focusHistoryID")}))'
}

hn_find_client() {
    local role="$1"
    hn_json_clients | python3 -c 'import json,sys
role=sys.argv[1]
for c in json.load(sys.stdin):
 if c.get("class")=="hyprnotes" and c.get("initialTitle","").startswith("hyprnotes-"+role+":"):
  print(json.dumps(c)); break' "$role"
}

hn_assert_host_unchanged() {
    local after
    after="$(hn_host_ws)"
    [ "$after" = "$HN_LIVE_HOST_WS" ] || {
        printf 'host workspace changed: before=%s after=%s\n' "$HN_LIVE_HOST_WS" "$after" >&2
        return 1
    }
    if [ -f "$HN_NESTED_DIR/expected-host-window-loss" ]; then
        printf 'nested compositor host window intentionally removed by ipc_loss check\n'
        return 0
    fi
    local nested_address
    nested_address="$(cat "$HN_NESTED_DIR/window" 2>/dev/null)"
    [ -n "$nested_address" ] || { printf 'nested host-window address missing\n' >&2; return 1; }
    hn_host_clients | python3 -c 'import json,sys
address=sys.argv[1]
clients=json.load(sys.stdin)
c=next((c for c in clients if c.get("address")==address),None)
assert c is not None, f"nested compositor host window missing: {address}"
assert c.get("workspace",{}).get("id")==4, f"nested compositor not parked on host workspace 4: {c}"
assert c.get("class")=="aquamarine", f"unexpected host class: {c}"
print(f"nested compositor parked on host workspace 4: {address}")' "$nested_address"
}

hn_live_finish() {
    local status="${1:-0}" stop_status=0
    [ "$HN_LIVE_FINALIZED" = 1 ] && return "$status"
    HN_LIVE_FINALIZED=1
    if [ "$HN_LIVE_INTERRUPTED" = 1 ]; then status=2; fi
    hn_live_ensure_session || return 2
    if [ "$HN_LIVE_STARTED" = 1 ] && [ "$HN_LIVE_INTERRUPTED" != 1 ]; then
        # Compositor is still owned here; verify host workspace and nested window placement first.
        hn_assert_host_unchanged >&2 || { [ "$status" -ge 1 ] || status=1; }
    fi
    local out
    out="$(python3 "$HN_LIVE_DIR/lifecycle.py" finish "$HN_LIVE_SESSION_DIR" 2>&1 >/dev/null)" || { stop_status=$?; printf 'controller finish: %s\n' "$out" >&2; }
    out="$(python3 "$HN_LIVE_DIR/lifecycle.py" verify-clean "$HN_LIVE_SESSION_DIR" 2>&1 >/dev/null)" || { stop_status=$?; printf 'controller verify-clean: %s\n' "$out" >&2; }
    if [ "$stop_status" -ne 0 ]; then
        printf 'SAFETY: controller receipt not clean; retaining durable evidence at %s\n' "$HN_LIVE_SESSION_DIR" >&2
        return 2
    fi
    [ "$status" -eq 2 ] && return 2
    return "$status"
}

hn_live_signal() {
    HN_LIVE_INTERRUPTED=1
    trap '' INT TERM
    hn_live_finish 2 || :
    exit 2
}
trap 'hn_live_signal' INT TERM
trap 'rc=$?; hn_live_finish "$rc"; exit $?' EXIT
