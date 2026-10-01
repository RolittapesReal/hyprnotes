# Hyprland integration snippet

Files: `hyprnotes.lua` (Lua config, Hyprland 0.56.2+), `hyprnotes.conf` (hyprlang, 0.53+ `windowrule = match:...` form).
Include ONE of them from your config (`dofile(...)` / `source = ...`). They only add app-specific rules and two example
bindings; animations, focus behavior and your other binds are untouched. Autostart is commented out: prefer the in-app
setting (XDG autostart, `hyprnotes --background`), or uncomment the `exec-once` / `hl.on("hyprland.start", ...)` line.

## How windows are matched
Qt sets one Wayland app-id per process (`hyprnotes`), so role + instance token are encoded in the first window title
(`hyprnotes-sticky:<16 hex>` / `hyprnotes-organizer:<16 hex>`) set before `show()`. Hyprland keeps it as `initialTitle`,
which rules match (`initial_title`) before the first frame, so stickies are floating from creation (no tiled flash).
The app then sets the real title; identity lookup uses pid + class + initialTitle, never the current title.

## Verified on this machine (Hyprland 0.56.2, Lua config, read-only / own window only)
- `Hyprland --verify-config -c hyprnotes.lua` -> `config ok`; the same command rejects an unknown rule field and an unknown
  `hl.dsp.*` function, so the check does exercise field names.
- `Hyprland --verify-config -c hyprnotes.conf` -> `config ok` (and rejects `bogus on`), so the hyprlang form parses.
- IPC syntax (`hyprctl dispatch` takes a Lua expression): `hl.dsp.window.float/pin({window='address:0x..',action='on'|'off'})`,
  `resize({..,x=,y=,exact=true})`, `move({..,x=,y=,exact=true})`, `move({..,workspace=N,follow=false})`. Driven against a window
  created by the live test (`HN_LIVE_HYPR=1 HN_LIVE_HYPR_WINDOW=1 QT_QPA_PLATFORM=wayland platform_test_live`): float, exact
  360x300 at (100,100), pin on/off all took effect and were read back via `j/clients`. `dispatch` replies `ok` even for a
  nonexistent address, so callers verify by re-querying rather than trusting the reply.
- Sockets: `$XDG_RUNTIME_DIR/hypr/$HYPRLAND_INSTANCE_SIGNATURE/.socket.sock` (`j/clients`, `/dispatch <lua>`), `.socket2.sock` events.

## NOT verified
- The window rules were not applied to the running compositor (that would change the live config). Verified only for syntax,
  not behaviorally: that a hyprnotes window opens floating at 360x300 with the rule active.
- `hyprnotes.conf` was verified by the parser only; the machine runs a Lua config so it was never loaded live.
- Key bindings `SUPER+N` / `SUPER+SHIFT+N` were not checked for conflicts with your own binds.
- keep-above: not provided (`supportsKeepAbove()` is false); `pin` only gives "all workspaces".
