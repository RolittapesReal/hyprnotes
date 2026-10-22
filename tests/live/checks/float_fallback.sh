#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/common.sh"
if [ "${1:-}" != --case ]; then
    base="$HN_TASK6_DIR"
    # Wrapper owns no session; each case finalizes its own. Never let the EXIT trap init one over $base.
    HN_LIVE_FINALIZED=1
    HN_LIVE_INVOCATION_OWNER_PID="${HN_LIVE_INVOCATION_OWNER_PID:-$BASHPID}"
    if [ -z "$HN_LIVE_EVIDENCE_DIR" ]; then
        HN_LIVE_EVIDENCE_DIR="$(mktemp -d /tmp/opencode/hyprnotes-live-${UID}.XXXXXX)" || exit 2
        chmod 700 "$HN_LIVE_EVIDENCE_DIR" || exit 2
    fi
    export HN_LIVE_EVIDENCE_DIR HN_LIVE_INVOCATION_OWNER_PID
    failed=0
    for setting in false true; do
        label=disabled; [ "$setting" = true ] && label=enabled
        case_status=0
        HN_TASK6_DIR="$base/$label" HN_LIVE_SESSION_DIR="" "$0" --case "$setting" || case_status=$?
        [ "$case_status" -eq 2 ] && exit 2
        [ "$case_status" -eq 0 ] || failed=1
    done
    exit "$failed"
fi
floating="${2:?expected setting value}"
HN_NESTED_DIR="$HN_TASK6_DIR"
export HN_NESTED_DIR HN_NESTED_WS=4 HN_NESTED_HOST_SIG HN_NESTED_RULES=0
mkdir -p "$HN_NESTED_DIR/app/hn-config"
printf '{"version":1,"windows":{"organizerFloating":%s}}\n' "$floating" >"$HN_NESTED_DIR/app/hn-config/config.json"
hn_live_begin 0
hn_start_app
hn_do_action --new-note
hn_wait_clients 1
hn_do_action --show-organizer
hn_wait_clients 2
python3 - "$HN_LIVE_RIG" "$floating" <<'PY'
import json, subprocess, sys
rig,want=sys.argv[1],sys.argv[2]=='true'
cs=json.loads(subprocess.check_output([rig,'hyprctl','-j','clients']))
sticky=next(c for c in cs if c.get('class')=='hyprnotes' and c.get('initialTitle','').startswith('hyprnotes-sticky:'))
organizer=next(c for c in cs if c.get('class')=='hyprnotes' and c.get('initialTitle','').startswith('hyprnotes-organizer:'))
assert sticky['floating'] is True, f'sticky fallback not floating: {sticky}'
assert organizer['floating'] is want, f'organizerFloating={want}, observed {organizer["floating"]}'
print(f'fallback sticky floating; organizerFloating={want} observed')
PY
