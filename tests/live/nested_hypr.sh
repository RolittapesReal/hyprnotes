#!/usr/bin/env bash
# Private nested Hyprland lifecycle adapter. Controller owns identity, signaling, and scratch removal.
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
LIFECYCLE="$HERE/lifecycle.py"
source "$HERE/preflight.sh"
unset HN_LIVE_PREFLIGHT_RESERVATION HN_LIVE_PREFLIGHT_PROBE
trap hn_preflight_release EXIT
N="${HN_NESTED_DIR:-/tmp/hn-nested}"
WS=4
SIZE="${HN_NESTED_SIZE:-1280x720@60}"
AQ="${HN_NESTED_AQ-AQ_NO_MODIFIERS=1 AQ_DRM_DEVICES=/dev/dri/card1}"
RT="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
HOST_SIG="${HN_NESTED_HOST_SIG:-${HYPRLAND_INSTANCE_SIGNATURE:-}}"
EVIDENCE_ROOT="${HN_LIVE_EVIDENCE_DIR:-/tmp/opencode/hyprnotes-live-$(id -u)}"
SESSION="${HN_LIVE_SESSION_DIR:-}"
OWNER_PID="${HN_LIVE_INVOCATION_OWNER_PID:-${PPID}}"
REQUESTED_OWNER_PID="${HN_LIVE_INVOCATION_OWNER_PID:-}"
LOOKUP=""

fail() { echo "nested_hypr: $*" >&2; return 2; }
die() { fail "$@"; exit 2; }
nested_sig() { cat "$N/sig" 2>/dev/null; }
host_ctl() { HYPRLAND_INSTANCE_SIGNATURE="$HOST_SIG" hyprctl "$@"; }
nest_ctl() { HYPRLAND_INSTANCE_SIGNATURE="$(nested_sig)" hyprctl "$@"; }
host_ws() { host_ctl -j activeworkspace | python3 -c 'import json,sys;print(json.load(sys.stdin)["id"])'; }
hypr_pid() { cat "$N/hypr.pid" 2>/dev/null; }
safe_path() { [[ "$1" =~ ^/[A-Za-z0-9_./-]+$ && "$1" != *..* ]]; }
valid_inputs() {
  safe_path "$N" || die "HN_NESTED_DIR must be an absolute path containing only [A-Za-z0-9_./-], without .."
  [[ "${HN_NESTED_WS:-4}" = 4 ]] || die "HN_NESTED_WS is fixed to authorized workspace 4"
  [[ "$SIZE" =~ ^[0-9]{3,5}x[0-9]{3,5}@[0-9]{1,3}([.][0-9]{1,3})?$ ]] || die "invalid HN_NESTED_SIZE"
  [[ "$AQ" =~ ^AQ_NO_MODIFIERS=1([[:space:]]AQ_DRM_DEVICES=/dev/dri/[A-Za-z0-9_-]+)?$ ]] || die "invalid HN_NESTED_AQ"
}
private_dir() { [ -d "$N" ] && [ ! -L "$N" ] && [ "$(stat -c %u:%a "$N" 2>/dev/null)" = "$(id -u):700" ]; }
ensure_evidence() {
  if [ ! -e "$EVIDENCE_ROOT" ]; then (umask 077; mkdir -m 700 -p "$EVIDENCE_ROOT") || return 1; fi
  [ -d "$EVIDENCE_ROOT" ] && [ ! -L "$EVIDENCE_ROOT" ] && [ "$(stat -c %u:%a "$EVIDENCE_ROOT" 2>/dev/null)" = "$(id -u):700" ] || return 1
  EVIDENCE_ROOT="$(realpath -e "$EVIDENCE_ROOT")" || return 1
  local nreal; nreal="$(realpath -m "$N")" || return 1
  case "$EVIDENCE_ROOT/" in "$nreal/"*) return 1;; esac
  case "$nreal/" in "$EVIDENCE_ROOT/"*) return 1;; esac
  LOOKUP="$EVIDENCE_ROOT/lookup-$(printf %s "$nreal" | sha256sum | cut -d' ' -f1).json"
}
load_lookup() {
  [ -e "$LOOKUP" ] || [ -L "$LOOKUP" ] || return 1
  [ -f "$LOOKUP" ] && [ ! -L "$LOOKUP" ] && [ "$(stat -c %u:%a "$LOOKUP")" = "$(id -u):600" ] || return 2
  local found; found="$(python3 - "$LOOKUP" "$(realpath -m "$N")" <<'PY'
import json,os,sys
try:
 def unique(pairs):
  out={}
  for k,v in pairs:
   if k in out: raise ValueError()
   out[k]=v
  return out
 d=json.load(open(sys.argv[1]),object_pairs_hook=unique); p=d["session"]
 if set(d)!={"session","scratch"} or not os.path.isabs(p) or d["scratch"]!=sys.argv[2]: raise ValueError()
 print(p)
except Exception: raise SystemExit(2)
PY
)" || return 2
  SESSION="$found"
}
write_lookup() {
  python3 - "$LOOKUP" "$SESSION" "$(realpath -m "$N")" <<'PY'
import json,os,sys
path,session,scratch=sys.argv[1:]
fd=os.open(path,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600)
with os.fdopen(fd,"w") as f:
 json.dump({"session":session,"scratch":scratch},f,sort_keys=True,separators=(",",":"));f.write("\n");f.flush();os.fsync(f.fileno())
fd=os.open(os.path.dirname(path),os.O_RDONLY|os.O_DIRECTORY);os.fsync(fd);os.close(fd)
PY
}
validate_session() {
  [ -n "$SESSION" ] || return 1
  local phase
  phase="$(python3 "$LIFECYCLE" status "$SESSION" 2>/dev/null)" || return 2
  python3 - "$SESSION/state.json" "$(realpath -m "$N")" <<'PY' || return 2
import json,os,stat,sys
try:
 p,scratch=sys.argv[1:]; st=os.stat(p,follow_symlinks=False)
 if not stat.S_ISREG(st.st_mode) or st.st_uid!=os.getuid() or stat.S_IMODE(st.st_mode)!=0o600: raise ValueError()
 d=json.load(open(p))
 if d.get("scratch")!=scratch: raise ValueError()
except Exception: raise SystemExit(2)
PY
  local stored_owner
  stored_owner="$(python3 - "$SESSION/state.json" <<'PY'
import json,sys
print(json.load(open(sys.argv[1]))["owner_pid"])
PY
)" || return 2
  [ -z "$REQUESTED_OWNER_PID" ] || [ "$REQUESTED_OWNER_PID" = "$stored_owner" ] || return 2
  OWNER_PID="$stored_owner"
  [ "$phase" = PREPARED ] || [ "$phase" = ACTIVE ] || [ "$phase" = CLEAN ] || [ "$phase" = PENDING ] || return 2
}
resolve_session() {
  ensure_evidence || return 2
  local supplied="$SESSION"
  if load_lookup; then
    [ -z "$supplied" ] || [ "$supplied" = "$SESSION" ] || return 2
  else
    local lookup_rc=$?
    [ "$lookup_rc" -eq 1 ] || return 2
    if [ -z "$supplied" ]; then SESSION=""; return 1; fi
    SESSION="$supplied"
  fi
  validate_session || return $?
}
init_session() {
  ensure_evidence || die "durable evidence directory invalid"
  if [ -z "$SESSION" ]; then
    if load_lookup; then validate_session || die "durable session binding invalid"
    else local lookup_rc=$?; [ "$lookup_rc" -eq 1 ] || die "durable session lookup malformed; preserving evidence"
      SESSION="$(python3 "$LIFECYCLE" init "$EVIDENCE_ROOT" "$N" "$OWNER_PID")" || die "controller session initialization failed"
      write_lookup || die "cannot commit durable scratch/session lookup"
    fi
  else
    validate_session || die "provided durable session binding invalid"
  fi
  export HN_LIVE_SESSION_DIR="$SESSION" HN_LIVE_INVOCATION_OWNER_PID="$OWNER_PID"
}
config_digest() {
  python3 - "$SESSION/state.json" <<'PY'
import json,sys
print(json.load(open(sys.argv[1]))["config_sha256"] or "")
PY
}
write_config() {
  {
    printf '%s\n' '-- nested test instance: no autostart, no binds, nothing that touches the host'
    printf '%s\n' 'hl.config({ misc = { force_default_wallpaper = 0, disable_hyprland_logo = true, disable_splash_rendering = true } })'
    printf 'hl.monitor({ output = "", mode = "%s", position = "auto", scale = "1" })\n' "$SIZE"
    if [ "${HN_NESTED_RULES:-0}" = 1 ]; then
      printf 'dofile(%s)\n' "$(python3 -c 'import json,sys;print(json.dumps(sys.argv[1]))' "$ROOT/packaging/share/hyprland/hyprnotes.lua")"
    fi
  } | python3 "$LIFECYCLE" config "$SESSION"
}
write_env() {
  local sig wayland
  sig="$(nested_sig)"; wayland="$(cat "$N/wayland" 2>/dev/null)"
  [ -n "$sig" ] && [ -n "$wayland" ] || return 1
  {
    printf 'export HN_NESTED_DIR=%q\nexport XDG_RUNTIME_DIR=%q\nexport HYPRLAND_INSTANCE_SIGNATURE=%q\nexport WAYLAND_DISPLAY=%q\n' "$N" "$RT" "$sig" "$wayland"
    printf 'export HN_LIVE_SESSION_DIR=%q\nexport HN_LIVE_INVOCATION_OWNER_PID=%q\n' "$SESSION" "$OWNER_PID"
    printf 'export HN_LIVE_EVIDENCE_DIR=%q\nexport QT_QPA_PLATFORM=wayland\n' "$EVIDENCE_ROOT"
  } > "$N/env.tmp" && chmod 600 "$N/env.tmp" && mv -f "$N/env.tmp" "$N/env"
}
find_instance() {
  local d
  for d in "$RT"/hypr/*/; do
    [ "$(head -1 "$d/hyprland.lock" 2>/dev/null)" = "$1" ] || continue
    basename "$d" > "$N/sig"; sed -n 2p "$d/hyprland.lock" > "$N/wayland"; return 0
  done
  return 1
}
write_launcher() {
  local exe="$1" cfg="$SESSION/hyprland.lua"
  python3 "$HERE/command_builders.py" nested-launcher "$N" "$SESSION" "$exe" "$AQ" -c "$cfg" > "$N/launch.sh.tmp" || return 1
  [ -s "$N/launch.sh.tmp" ] && chmod 700 "$N/launch.sh.tmp" && mv -f "$N/launch.sh.tmp" "$N/launch.sh"
}
cleanup_after_start_failure() {
  local original="$1"
  python3 "$LIFECYCLE" finish "$SESSION" >/dev/null 2>&1 || { echo "nested_hypr: controller finalization uncertain; durable evidence retained" >&2; return 2; }
  return "$original"
}
intent_compositor() {
  local exe="$1" cfg="$SESSION/hyprland.lua" digest request
  digest="$(config_digest)" || return 1
  request="$(python3 - "$exe" "$cfg" "$digest" <<'PY'
import json,sys
exe,cfg,digest=sys.argv[1:]
print(json.dumps({"argv":[exe,"-c",cfg],"config_sha256":digest},separators=(",",":")))
PY
)" || return 1
  printf '%s\n' "$request" | python3 "$LIFECYCLE" intent "$SESSION" compositor "$exe"
}
start_once() {
  valid_inputs; hn_preflight_release
  mkdir -p "$(dirname "$N")" || die "cannot create scratch parent"
  hn_preflight_scratch "$(dirname "$N")" || die "scratch preflight failed; refusing GUI startup"
  (umask 077; mkdir -m 700 "$N") 2>/dev/null || { private_dir || die "nested scratch must be private and owned"; }
  private_dir || die "nested scratch must be private and owned"
  init_session
  local phase; phase="$(python3 "$LIFECYCLE" status "$SESSION")" || die "controller session validation failed"
  if [ "${HN_NESTED_RESTART:-0}" = 1 ]; then
    [ "$phase" = ACTIVE ] || die "restart requires an ACTIVE session ($phase)"
  else
    [ "$phase" = PREPARED ] || die "session is already in use or unsafe ($phase); refusing relaunch"
    write_config || die "durable config commit failed"
  fi
  hn_preflight_workspace4 "$HOST_SIG" "$WS" || die "workspace 4 preflight failed; refusing GUI startup"
  local prev_ws; prev_ws="$(host_ws)" || die "cannot snapshot host workspace"
  local exe dispatch_output intent_id
  exe="$(command -v Hyprland)" || die "Hyprland is unavailable"
  exe="$(realpath -e "$exe")" || die "cannot resolve Hyprland executable"
  write_launcher "$exe" || die "cannot write private launcher"
  [ "${HN_NESTED_RULE_LAUNCH:-1}" = 1 ] || die "workspace-targeted launch disabled; refusing direct-launch fallback"
  local dispatch_expr; dispatch_expr="$(python3 "$HERE/command_builders.py" dispatch "$N/launch.sh")" || die "cannot encode launcher dispatch"
  intent_id="$(intent_compositor "$exe")" || { echo "nested_hypr: controller refused compositor intent; no dispatch" >&2; return 2; }
  [ -n "$intent_id" ] || { echo "nested_hypr: controller returned empty intent; no dispatch" >&2; return 2; }
  dispatch_output="$(host_ctl dispatch "$dispatch_expr" 2>&1)" || { echo "nested_hypr: dispatch unconfirmed after committed intent" >&2; return 2; }
  [ "$dispatch_output" = ok ] || { echo "nested_hypr: dispatch response unconfirmed after committed intent" >&2; return 2; }
  local i pid
  for i in $(seq 1 100); do [ -s "$N/hypr.pid" ] && break; sleep .05; done
  [ -s "$N/hypr.pid" ] || { echo "nested_hypr: launcher PID missing after dispatch; intent retained" >&2; return 2; }
  pid="$(hypr_pid)"; [[ "$pid" =~ ^[1-9][0-9]*$ ]] || { echo "nested_hypr: launcher PID malformed; intent retained" >&2; return 2; }
  # Launcher publishes its PID before exec; identity validation is strict, so retry only
  # the same verification (pending intent stays unconsumed on failure) for a bounded 5 s.
  local recorded_comp=""
  for i in $(seq 1 100); do
    if python3 "$LIFECYCLE" record "$SESSION" compositor "$pid" >/dev/null 2>&1; then recorded_comp=1; break; fi
    sleep .05
  done
  [ -n "$recorded_comp" ] || { echo "nested_hypr: compositor identity unverified; intent retained" >&2; return 2; }
  local addr="" placed
  for i in $(seq 1 100); do
    # find_instance returns 1 until the nested instance lock exists; errexit must not abort start.
    [ -s "$N/sig" ] || find_instance "$pid" || true
    addr="$(host_ctl -j clients | python3 -c 'import json,sys
for c in json.load(sys.stdin):
 if c.get("pid")==int(sys.argv[1]): print(c.get("address",""));break' "$pid")" || addr=""
    [ -n "$addr" ] && break
    sleep .05
  done
  [ -n "$addr" ] || { echo "nested window did not appear; invoking controller cleanup" >&2; cleanup_after_start_failure 1; return $?; }
  printf '%s\n' "$addr" > "$N/window"
  # Instance lock files appear slightly after the host window; bounded wait (5 s) before giving up.
  for i in $(seq 1 100); do
    [ -s "$N/sig" ] && [ -s "$N/wayland" ] && break
    find_instance "$pid" || true
    sleep .05
  done
  [ -s "$N/sig" ] && [ -s "$N/wayland" ] || { echo "nested_hypr: nested instance signature/wayland socket not found" >&2; cleanup_after_start_failure 1; return $?; }
  write_env || { echo "nested_hypr: cannot write nested env" >&2; cleanup_after_start_failure 1; return $?; }
  for i in $(seq 1 100); do nest_ctl -j monitors 2>/dev/null | grep -q '"name"' && break; sleep .05; done
  nest_ctl -j monitors 2>/dev/null | grep -q '"name"' || { echo "nested_hypr: nested IPC not answering" >&2; cleanup_after_start_failure 1; return $?; }
  placed="$(host_ctl -j clients | python3 "$HERE/placement_gate.py" "$addr" "$pid")" || { echo "cannot verify exact workspace-4 placement; no move/focus fallback" >&2; cleanup_after_start_failure 2; return $?; }
  [ "$placed" = 4 ] || { echo "INCIDENT: expected host workspace 4, got $placed; no move/focus fallback" >&2; cleanup_after_start_failure 2; return $?; }
  [ "$(host_ws)" = "$prev_ws" ] || { echo "host workspace changed; refusing host focus fallback" >&2; cleanup_after_start_failure 2; return $?; }
  echo "started via=rule session=$SESSION sig=$(nested_sig) wayland=$(cat "$N/wayland") window=$addr host-workspace=$placed"
}
cmd_start() {
  command -v Hyprland >/dev/null && command -v hyprctl >/dev/null || die "Hyprland/hyprctl not installed"
  [ -n "$HOST_SIG" ] && [ -n "${WAYLAND_DISPLAY:-}" ] || die "host Wayland/Hyprland session unavailable"
  valid_inputs
  ensure_evidence || die "durable evidence directory invalid"
  if resolve_session; then :
  else
    local resolve_rc=$?
    [ "$resolve_rc" -eq 1 ] || die "durable session resolution failed"
    [ ! -e "$N" ] && [ ! -L "$N" ] || die "scratch exists without durable session binding; refusing ownership reconstruction"
  fi
  if [ -n "$SESSION" ]; then
    local phase; phase="$(python3 "$LIFECYCLE" status "$SESSION" 2>/dev/null)" || die "durable session invalid"
    [ "$phase" = ACTIVE ] && { echo "nested session already active: $SESSION"; return 0; }
    [ "$phase" = PREPARED ] || die "session phase $phase refuses new compositor launch"
  fi
  start_once
}
cmd_restart() {
  # Replace a compositor that was lost (host window removed) inside the SAME controller session.
  # The previous compositor must already be gone; its record stays for exit proof at finish.
  command -v Hyprland >/dev/null && command -v hyprctl >/dev/null || die "Hyprland/hyprctl not installed"
  [ -n "$HOST_SIG" ] && [ -n "${WAYLAND_DISPLAY:-}" ] || die "host Wayland/Hyprland session unavailable"
  valid_inputs
  ensure_evidence || die "durable evidence directory invalid"
  resolve_session || die "no unambiguous durable session; refusing restart"
  local old; old="$(hypr_pid)"
  [[ "$old" =~ ^[1-9][0-9]*$ ]] || die "previous compositor PID missing; refusing restart"
  if [ -e "/proc/$old" ] && ! grep -q '^State:.*Z' "/proc/$old/status" 2>/dev/null; then
    die "previous compositor still running; refusing restart"
  fi
  rm -f "$N/hypr.pid" "$N/sig" "$N/wayland" "$N/window" "$N/env"
  HN_NESTED_RESTART=1 start_once
}
cmd_stop() {
  resolve_session || { echo "nested_hypr: no unambiguous validated durable session; refusing stop" >&2; return 2; }
  python3 "$LIFECYCLE" finish "$SESSION" || { echo "nested_hypr: controller refused cleanup; durable evidence retained" >&2; return 2; }
  echo "stopped session=$SESSION"
}
active_session() {
  resolve_session || return 2
  local phase; phase="$(python3 "$LIFECYCLE" status "$SESSION" 2>/dev/null)" || return 2
  [ "$phase" = ACTIVE ]
}
cmd_status() {
  active_session || { echo "nested_hypr: no validated active session" >&2; return 2; }
  nest_ctl -j monitors >/dev/null || return 2
  echo "session=$SESSION pid=$(hypr_pid) sig=$(nested_sig) wayland=$(cat "$N/wayland" 2>/dev/null) window=$(cat "$N/window" 2>/dev/null) host-workspace-now=$(host_ws)"
}
cmd_run() {
  [ "$#" -gt 0 ] || die "run requires executable"
  active_session || die "not running or session ownership unavailable"
  local exe="$1"; shift
  if [[ "$exe" != /* ]]; then exe="$(command -v "$exe")" || die "app executable unavailable"; fi
  exe="$(realpath -e "$exe")" || die "app executable cannot be resolved"
  [ -x "$exe" ] || die "app executable is not executable"
  local digest request
  digest="$(config_digest)" || die "durable config unavailable"
  request="$(python3 - "$exe" "$digest" "$@" <<'PY'
import json,sys
exe,digest,*args=sys.argv[1:]
print(json.dumps({"argv":[exe,*args],"config_sha256":digest},separators=(",",":")))
PY
)" || die "cannot encode app intent"
  printf '%s\n' "$request" | python3 "$LIFECYCLE" intent "$SESSION" app "$exe" >/dev/null || die "controller refused app intent"
  local app="$N/app" p rc
  mkdir -p "$app"/{home,config,data,state,cache,hn-config,hn-data,hn-state,hn-cache,hn-notes} || die "cannot create app scratch"
  (
    . "$N/env"
    export HOME="$app/home" XDG_CONFIG_HOME="$app/config" XDG_DATA_HOME="$app/data" XDG_STATE_HOME="$app/state" XDG_CACHE_HOME="$app/cache"
    export HN_CONFIG_DIR="$app/hn-config" HN_DATA_DIR="$app/hn-data" HN_STATE_DIR="$app/hn-state" HN_CACHE_DIR="$app/hn-cache" HN_NOTES_DIR="$app/hn-notes"
    export HN_LIVE_OWNER_TOKEN="$(< "$SESSION/owner-token")"
    exec "$exe" "$@"
  ) &
  p=$!
  local i recorded="" rec_err=""
  for i in $(seq 1 20); do
    if rec_err="$(python3 "$LIFECYCLE" record "$SESSION" app "$p" 2>&1 >/dev/null)"; then recorded=1; break; fi
    sleep .01
  done
  if [ -z "$recorded" ]; then
    # A known one-shot client action (a single --new-note/--show-organizer argument) can exit before
    # it is snapshotted. Only if it is our own child, has ALREADY exited and is reaped here may the
    # controller resolve the intent. Anything else (other args, still running, identity mismatch)
    # keeps the intent pending and never falls back to numeric-PID cleanup.
    local pst=""
    if [ "$#" -eq 1 ] && { [ "$1" = --new-note ] || [ "$1" = --show-organizer ]; }; then
      pst="$(sed -n 's/^State:[[:space:]]*\(.\).*/\1/p' "/proc/$p/status" 2>/dev/null || true)"
      if [ -z "$pst" ] || [ "$pst" = Z ]; then
        wait "$p" && rc=0 || rc=$?   # errexit-safe: client may exit non-zero
        if python3 "$LIFECYCLE" oneshot-exited "$SESSION" "$p" >/dev/null 2>&1; then return "$rc"; fi
      fi
    fi
    echo "nested_hypr: app identity unverified; intent retained; no numeric-PID cleanup (last record error: $rec_err)" >&2; return 2
  fi
  wait "$p" && rc=0 || rc=$?
  return "$rc"
}
cmd_check_ipc() {
  active_session || die "not running (start first)"
  [ "${HN_NESTED_RULES:-0}" != 1 ] || die "check-ipc needs nested config without app float rules"
  local bin="${HN_APP_BIN:-$ROOT/build-final-chk/src/app/hyprnotes}" a="$N/app" out="" i addr float
  [ -x "$bin" ] || die "app binary not found: $bin (set HN_APP_BIN)"
  [ -z "$(nest_notes)" ] || die "nested instance already shows hyprnotes windows; stop and start it fresh"
  cmd_run "$bin" --background > "$N/ipc-app.log" 2>&1 &
  sleep 2
  cmd_run "$bin" --new-note > "$N/ipc-new.log" 2>&1 || return 1
  for i in $(seq 1 100); do out="$(nest_notes)"; [ -n "$out" ] && break; sleep .1; done
  [ -n "$out" ] || { echo "FAIL: no hyprnotes window in nested instance"; return 1; }
  for i in $(seq 1 20); do out="$(nest_notes | head -1)"; [ "${out#* }" != "${out#* True}" ] && break; sleep .1; done
  read -r addr float _ <<< "$out"
  echo "sticky in nested instance: $out"
  [ "$float" = True ] || { echo "FAIL: sticky is tiled; app IPC did not reach nested instance"; return 1; }
  nest_ctl dispatch "hl.dsp.window.close({window='address:$addr'})" >/dev/null
  for i in $(seq 1 20); do nest_notes | grep -q "$addr" || break; sleep .1; done
  nest_notes | grep -q "$addr" && { echo "FAIL: window did not close"; return 1; }
  cmd_run "$bin" --new-note > "$N/ipc-new2.log" 2>&1 || return 1
  for i in $(seq 1 30); do out="$(nest_notes | head -1)"; [ -n "$out" ] && break; sleep .1; done
  [ -n "$out" ] || { echo "FAIL: app did not show new sticky after compositor close"; return 1; }
  for i in $(seq 1 20); do out="$(nest_notes | head -1)"; [ "${out#* }" != "${out#* True}" ] && break; sleep .1; done
  [ "${out#* }" != "${out#* True}" ] || { echo "FAIL: second sticky not floated: $out"; return 1; }
  echo "PASS: app IPC placed stickies in nested instance and survived compositor-side close"
}
nest_notes() {
  nest_ctl -j clients | python3 -c 'import json,sys
for c in json.load(sys.stdin):
 if c.get("class")=="hyprnotes": print(c["address"],c["floating"],c["workspace"]["id"])'
}

case "${1:-}" in
  start) cmd_start ;;
  restart) cmd_restart ;;
  stop) cmd_stop ;;
  status) cmd_status ;;
  hyprctl) shift; active_session || die "not running"; nest_ctl "$@" ;;
  env) active_session || die "not running"; cat "$N/env" ;;
  run) shift; cmd_run "$@" ;;
  check-ipc) cmd_check_ipc ;;
  *) sed -n '2,12p' "${BASH_SOURCE[0]}"; exit 2 ;;
esac
