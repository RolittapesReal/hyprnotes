# Hyprnotes native mods (API v1)

Native mods are shared libraries written in C, C++ or Rust. They add commands inside the Hyprnotes process, so they are not sandboxed and can crash the app. They are unrelated to Hyprland compositor plugins. Hyprnotes does not provide a scripting runtime for native mods.

## Files
- Header: `src/mods/include/hyprnotes/mod_api.h` (pure C, `HN_MOD_API_VERSION 1`).
- Install location: `$XDG_DATA_HOME/hyprnotes/mods/<id>/` (default `~/.local/share/hyprnotes/mods/<id>/`) containing `mod.json` and the library.
- Enabled list: `mods-enabled.json` in the Hyprnotes config dir: `{"version":1,"enabled":["my-mod"]}`. Installed mods load only when their id is listed as enabled.

## mod.json
```json
{
  "id": "uppercase-selection",        // [a-z0-9._-]{1,64}, must equal the directory name
  "name": "Uppercase selection",
  "version": "1.0.0",
  "host_api_version": 1,              // must equal the host's version, otherwise rejected before any dlopen
  "arch": "x86_64",                   // must equal the host architecture
  "library": "libuppercase_selection.so",  // relative path inside the mod directory
  "entry": "hn_mod_entry",            // optional, C symbol
  "activation": ["on-command:uppercase-selection"],  // on-startup | on-note-open | on-command:<id>
  "commands": [{"id": "uppercase-selection", "title": "Uppercase selection"}]  // optional, lets palettes list commands before activation
}
```
(Comments above are for illustration; real JSON has none.) Validation errors are reported as a `ModError` list; one bad mod never blocks others. Scanning a mod reads only `mod.json`.

## Lifecycle
1. App start: manifests are scanned, the enabled list is read. No library is loaded except for enabled `on-startup` mods.
2. The library is `dlopen`ed lazily when its activation condition is reached (command run, first note opened, startup).
3. The host calls `hn_mod_entry(host, out)`. The module checks `HN_HOST_COMPATIBLE(host)`, registers commands/subscriptions, and fills `out` (`struct_size`, `api_version`, `name`, optional `shutdown`). The host rejects the module (and unloads it, discarding its registrations) if the status is not `HN_OK`, the `api_version` differs, or `struct_size` is smaller than `HN_MOD_API_V1_SIZE`. A failed mod is not retried in the same session.
4. Enable/disable takes effect on **restart only**. There is no hot unload; an already loaded library stays until exit. Disabled mods are never loaded and leave no library, callback or timer resident.

## Host API summary
`register_command`, `log`, `get_selection_text` (returns a heap copy; release with `free_buffer`), `begin_transaction`/`replace_selection`/`insert_text`/`end_transaction`, `subscribe` (note-opened, note-saved, note-closed, selection-changed), `schedule_work` (host-run one-shot on the UI thread), `free_buffer`. Pass `host->handle` as the first argument to every call.

- Edits go through host transactions: one begin/end pair is one undo step and triggers autosave. Mutations outside a transaction return `HN_ERR_STATE`; without a bound document `HN_ERR_NO_DOCUMENT`. A transaction left open by a callback is closed by the host with a warning.
- Registration (`register_command`, `subscribe`) is only allowed inside `hn_mod_entry`.
- Events are coalesced by the host (selection-changed and note-saved: latest per note; opened/closed are not dropped) and delivered without a bound document. The host allocates or queues no event when no loaded mod has subscribed.
- All calls happen on the UI thread. Do not create threads or polling timers. Callbacks longer than 4 ms are logged and counted in the host statistics. For longer work, copy the text into an immutable snapshot and use `schedule_work`.
- Ownership: strings passed to callbacks are valid only during the call; buffers returned by the host belong to the caller until `free_buffer`. UTF-8 everywhere. No C++/Qt/Rust types or exceptions cross the boundary.

## Example
`examples/mods/uppercase-selection/` builds standalone (`cmake -S . -B build && cmake --build build`, needs only a C compiler) and adds the `uppercase-selection` command (ASCII letters only). To try it: copy `mod.json` and the `.so` to `~/.local/share/hyprnotes/mods/uppercase-selection/` and add the id to the enabled list.

## Host side (for app integrators)
`hn::mods::ModHost` (`hn/mods/mods.h`): `start(modsDir, enabledPath)`, `commands()`, `runCommand(id, DocumentBridge*)`, `post(EventType, noteId)`, `shutdown()`. Each window implements `DocumentBridge` (selectionText, begin/replace/insert/end) on top of the editor's transaction history.

## Limits
Native mods are not sandboxed, and the host cannot limit their CPU or memory use. The ABI is stable only on x86_64 Linux. Mod memory remains allocated until restart. Mods currently add commands; panels and menus are not available.
