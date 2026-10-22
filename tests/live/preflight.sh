#!/usr/bin/env bash
# Fail-closed scratch capacity and workspace checks shared by live entry points.
set -euo pipefail

hn_preflight_scratch() {
    local parent="$1" probe bytes=${HN_LIVE_RESERVE_BYTES:-67108864} files=${HN_LIVE_RESERVE_FILES:-128}
    [ -d "$parent" ] && [ -w "$parent" ] || { echo "INCONCLUSIVE: scratch parent unavailable or unwritable: $parent" >&2; return 1; }
    probe="$(mktemp -d "$parent/.hn-preflight.XXXXXX")" || { echo "INCONCLUSIVE: cannot create scratch probe under $parent" >&2; return 1; }
    if ! ( umask 077; mkdir "$probe/tree" && printf 'preflight\n' >"$probe/tree/config" && printf 'header\n' >"$probe/tree/header" && touch "$probe/tree/marker" ); then
        rm -rf -- "$probe"; echo "INCONCLUSIVE: scratch mkdir/config/header/touch probe failed under $parent" >&2; return 1
    fi
    local i
    for ((i=0; i<files; i++)); do
        if ! : >"$probe/tree/inode-$i"; then
            rm -rf -- "$probe"; echo "INCONCLUSIVE: scratch inode/quota probe failed under $parent after $i files" >&2; return 1
        fi
    done
    if ! fallocate -l "$bytes" "$probe/reservation" 2>/dev/null; then
        rm -rf -- "$probe"; echo "INCONCLUSIVE: cannot reserve ${bytes} bytes on scratch filesystem (space/quota) under $parent" >&2; return 1
    fi
    # Keep reservation until caller cleanup; this converts later quota exhaustion into reserved headroom.
    HN_LIVE_PREFLIGHT_RESERVATION="$probe/reservation"
    HN_LIVE_PREFLIGHT_PROBE="$probe"
}

hn_preflight_release() {
    if [ -n "${HN_LIVE_PREFLIGHT_PROBE:-}" ]; then rm -rf -- "$HN_LIVE_PREFLIGHT_PROBE"; fi
}

hn_preflight_workspace4() {
    local sig="$1" ws="$2"
    [ "$ws" = 4 ] || { echo "INCONCLUSIVE: live harness is authorized only for host workspace 4" >&2; return 1; }
    HYPRLAND_INSTANCE_SIGNATURE="$sig" hyprctl -j workspaces 2>/dev/null |
        python3 -c 'import json,sys; rows=json.load(sys.stdin); sys.exit(0 if any(int(x.get("id",-1))==4 for x in rows) else 1)' || {
            echo "INCONCLUSIVE: host workspace 4 unavailable; refusing launch without switching host workspace" >&2; return 1;
        }
}
