# Live Hyprland tests

The live checks launch Hyprland as a nested window inside an existing Wayland session. `nested_hypr.sh run` redirects HOME, XDG, Hyprnotes, and note storage paths into scratch space, so the app uses isolated state. At its initial map, the host's silent exec dispatch must place the nested compositor on workspace 4. That workspace must already exist; the harness does not switch the host's active workspace. If it cannot control placement or scratch preflight fails, it stops before launching a GUI. Read the script comments before changing its environment.

## Prerequisites

- Run from a Hyprland Wayland session with `Hyprland`, `hyprctl`, and `wayland-info`-compatible Qt dependencies available.
- Build the test-only driver with `HN_LIVE_TESTS=ON`; the usual binary is `build-stress/tests/live/hyprnotes_live_driver`.
- A nested monitor backend must work. The helper defaults `HN_NESTED_AQ` to `AQ_NO_MODIFIERS=1 AQ_DRM_DEVICES=/dev/dri/card1`; set it to values appropriate for your machine if needed.
- Keep the host compositor and its active workspace available. The helper does not use the host's Hyprland instance signature for nested commands.

Build the test driver and run the helper's standalone IPC check:

```sh
cmake -S . -B build-live -G Ninja -DHN_LIVE_TESTS=ON
cmake --build build-live
export HN_LIVE_DRIVER_BIN="$PWD/build-live/tests/live/hyprnotes_live_driver"
export HN_APP_BIN="$PWD/build-live/src/app/hyprnotes"
tests/live/nested_hypr.sh start
tests/live/nested_hypr.sh check-ipc
tests/live/nested_hypr.sh stop
```

To use nested `hyprctl` from a shell, run `eval "$(tests/live/nested_hypr.sh env)"` only in a subshell or disposable test shell. The helper command `tests/live/nested_hypr.sh hyprctl ...` targets the nested compositor directly. `HN_NESTED_DIR` defaults to `/tmp/hn-nested`; set it to a private scratch path for parallel runs.

## Full live matrix

The shared helper starts and stops a nested compositor for each matrix check. Do not start another rig before running the matrix. Build the app and driver first, then run the full matrix or choose checks:

```sh
tests/live/matrix.sh
tests/live/matrix.sh sticky_lifecycle ipc_loss
```

Before creating per-check directories, the matrix checks that workspace 4 exists and scratch space has room, inodes, and a quota-aware block reservation. It records the host's active workspace before and after; it does not switch workspaces. The nested compositor is launched only through the silent workspace-targeted exec dispatch. If dispatch fails, the matrix stops before compositor startup and does not try a direct-launch or move fallback. Exit status `2` means a check refused to continue safely; the matrix marks that run inconclusive and fails instead of skipping the check. The matrix covers rules and fallback, sticky lifecycle, focus and stacking, workspaces, monitors and scales, tray and Waybar, binds, IPC loss and malformed messages, event storms, and reload and monitor changes. Individual scripts may have further compositor requirements.

The helper's `check-ipc` needs a fresh nested instance with no Hyprnotes windows and with app float rules disabled (`HN_NESTED_RULES` must not be `1`). It checks that sticky placement reached the nested compositor, closes a window through the compositor, then verifies that the app can show another. The app remains running after the check; `stop` closes it.

## Session controller and cleanup

`lifecycle.py` records every nested compositor and app launch in a durable session. It first records an `intent`, then identifies the process with `record`. Cleanup signals only recorded processes, using a pidfd after checking UID, start time, executable, arguments, and owner token again. It has no numeric-PID, process-group, or `pkill` fallback. `finish` stops recorded processes in reverse order, waits up to 15 seconds for each to exit, removes the scratch tree, and writes a receipt for `verify-clean`. Without a clean receipt, the matrix result is inconclusive.

Two controller operations exist only for the test rig:

- `oneshot-exited` resolves the pending intent of a lone `--new-note` or `--show-organizer` client that exited before it could be snapshotted. It refuses anything else, and refuses while the PID is still in `/proc`.
- `reconfig` replaces the nested compositor config (used by the scale and reload checks) and moves the pinned digest with the file.

`nested_hypr.sh restart` relaunches a lost compositor inside the same session; it refuses while the previous one is still running.

App state lives under `$HN_NESTED_DIR/app/` and can be discarded. If a run is interrupted, its evidence stays in the evidence directory (`/tmp/opencode/hyprnotes-live-<uid>.*` by default). Do not kill host processes based on a matching name. For an unresolved session, inspect `state.json` and use the recorded rows with the guard, or request a review before cleanup.

## Private D-Bus and display loss

The nested compositor is a Wayland client of the host and reuses its runtime directory. Hyprnotes data stays in scratch HOME, XDG, and HN directories. The `tray_waybar` check runs its tray probe under `dbus-run-session`, which keeps StatusNotifier traffic on a private bus for that check. Do not replace `DBUS_SESSION_BUS_ADDRESS` in the host shell or run the probe outside its wrapper. Other checks do not create or need a private bus.

If the host Wayland display disappears, the live run is interrupted and its result is invalid. Test cleanup must not restart the host compositor, Hyprnotes, or another user process. When the original session is available again, inspect helper state and logs, then use the scoped `stop` command above. Restarting the host display session is outside this procedure.

## Workspace 4 and a floating holder

Preflight requires workspace 4 to exist and does not create it. If workspace 4 is empty, first open a small floating window there (for example, start a terminal with `hl.dsp.exec_cmd(..., {workspace="4 silent"})` and float it). Run the matrix, then close the holder window. A tiled window halves the nested output and causes the rule-size check to fail.

## Recorded results

- 2026-10-15, Qt 6.12 Release build, Hyprland 0.56.2: the 13-check matrix passed in two separate invocations, each exiting 0, with the host workspace unchanged and no processes left over.
- The runs exposed races the fixture suites had missed: the instance lock appeared late, one-shot clients exited before identification, and `finish` did not wait long enough. The fixes are covered by `lifecycle_oneshot_selftest.py` and the chain suite.
- These results apply to the recorded runs. They do not establish behavior on other Hyprland versions, hardware, or compositors. Offline fixture suites (`lifecycle_selftest.py`, `lifecycle_chain_selftest.py`, `process_guard_selftest.py`, `preflight_selftest.sh`, and related checks) are separate gates.
