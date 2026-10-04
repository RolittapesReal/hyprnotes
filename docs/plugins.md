# Writing Hyprnotes plugins

Plugins let you change how Hyprnotes works: add commands, expand text as you type, react to saving a note, tidy text on save, talk to a web service. Most plugins are one small Lua file.

> **Third-party plugins are not reviewed by the Hyprnotes project. A plugin can read or change your notes and, depending on its permissions, send data over the network. Only install plugins from sources you trust.**
>
> For native plugins the app adds: **This plugin runs native code with full access to your user account. It is not sandboxed.**

That warning is shown, always visible, in the dialog where you approve a plugin. Read [the security model](#security-model-and-its-limits) before you publish or install anything.

Companion documents: [plugin-api.md](plugin-api.md) (every `hn.*` function), [examples/plugins](../examples/plugins) (ten complete plugins).

**Official plugins.** Hyprnotes also maintains four plugins of its own, in the separate `hyprnotes-plugins` repository (next to this one): Slash Menu, Journal and Templates, Tasks and Saved Queries, and Wiki Links. They use the same API and permissions as yours, so they double as larger examples. They are not bundled with the app; the plugin marketplace will carry them.

## Two kinds of plugin

| | Script plugin (this guide) | Native plugin |
|---|---|---|
| Language | Lua 5.5 in a sandbox | C/C++/Rust shared library (see [mods.md](mods.md)) |
| Sandboxed | yes, best effort | **no** |
| Needs approval | yes | yes, with the extra native warning |
| Hot reload | yes | restart |

Everything below is about script plugins.

## Quickstart: your first plugin in 5 minutes

The command-line steps below need the `hyprnotes` binary with the plugin authoring flags (the same functions are in the `hn_plugins` library: `createTemplate`, `checkDirectory`, `packDirectory`).

1. **Scaffold** a working plugin:

   ```bash
   hyprnotes --new-plugin Shout
   ```

   This creates a folder with `plugin.json` and `main.lua`; the plugin id is derived from the name (`shout`). The rest of this page calls that folder `~/shout`.

2. **Look at `main.lua`**. The scaffold registers one command that upper-cases the selection:

   ```lua test perms=note.read,note.edit,ui
   hn.command{
     id = "shout",
     title = "Shout selection",
     run = function()
       local sel = hn.note.selection()            -- permission: note.read
       if sel == "" then
         hn.ui.notify("Select some text first")   -- permission: ui
         return
       end
       hn.note.replace_selection(sel:upper())     -- permission: note.edit (one undo step)
     end,
   }
   ```

3. **Check it**:

   ```bash
   hyprnotes --check-plugin ~/shout
   ```

   It validates the manifest, file sizes, UTF-8, Lua syntax and prints warnings for dangerous permissions. Exit code 0 means installable.

4. **Install it**: open **Plugins** in Hyprnotes, choose *Install from folder* and pick `~/shout`. A new plugin is **disabled until you approve it**. The approval dialog shows the author, the folder, a SHA-256 of the files, every permission in plain language (dangerous ones highlighted) and the warning above. Approve, and the command *Shout selection* appears in the command palette.

5. **Edit and reload**. Change the file, press **Reload** on the plugin's card, run the command again. No app restart. Approval is tied to the exact files: after an edit, the plugin keeps running only if the reload is a developer reload and your `permissions` list did not grow beyond what was approved; otherwise it waits for you to approve it again.

6. **Share it**:

   ```bash
   hyprnotes --pack-plugin ~/shout      # writes a .hnplugin archive
   ```

Try the finished examples in `examples/plugins/` (`word-count`, `insert-date`, `sort-lines`, `markdown-toc`, `title-case-selection`, `daily-note`, `snippets`, and the API-2 examples `backlinks-panel`, `link-completion`, `open-tasks`): each is a folder with `plugin.json`, `main.lua` and a README, and each is installed and run by the test-suite.

## Anatomy of a plugin

```
my-plugin/
  plugin.json     required: the manifest
  main.lua        the entry file (name set by "entry")
  lib/util.lua    optional modules, loaded with require("lib.util")
  README.md       optional, shown nowhere by the app but nice for people
```

### plugin.json

```json manifest
{
  "id": "word-count",
  "name": "Word Count",
  "version": "1.0.0",
  "author": "Your Name",
  "description": "Counts words in the selection or the note.",
  "homepage": "https://example.com/word-count",
  "api": 1,
  "tier": "script",
  "entry": "main.lua",
  "permissions": ["note.read", "ui"],
  "min_app": "0.1.0"
}
```

| Field | Required | Rules |
|---|---|---|
| `id` | yes | 1-64 characters of `a-z 0-9 . _ -`, starting with a letter or digit. Must equal the folder name. Becomes the install folder and the prefix of command ids (`word-count:count`). |
| `name` | yes | up to 80 characters |
| `version` | yes | `MAJOR.MINOR.PATCH`, for example `1.2.0` (an optional `-beta.1` suffix is allowed) |
| `author` | yes | up to 80 characters, shown in the approval dialog |
| `description` | yes | up to 500 characters |
| `homepage` | no | an http(s) URL |
| `api` | yes | the integer `1` (the original API) or `2` (adds the note index, panels, completion and link handlers; see [Plugin API 2](#plugin-api-2-panels-completion-and-the-note-index)). API-2 permissions in an `"api": 1` manifest are rejected |
| `tier` | yes | `"script"` here; `"native"` for compiled plugins |
| `entry` | no | relative path to the Lua file, default `main.lua` |
| `permissions` | yes | array of permission names; `[]` for none; no duplicates |
| `net_hosts` | with `network` | up to 16 exact lower-case public host names (no scheme, port, wildcard, IP address or local name); required when you ask for `network` and rejected when you do not |
| `min_app` | no | oldest Hyprnotes version that can run the plugin |

A plugin that talks to a web service looks like this:

```json manifest
{
  "id": "quote-of-the-day",
  "name": "Quote of the day",
  "version": "0.2.0",
  "author": "Your Name",
  "description": "Inserts a quote fetched from quotes.example.com.",
  "api": 1,
  "tier": "script",
  "permissions": ["note.edit", "network"],
  "net_hosts": ["quotes.example.com"]
}
```

Package limits (enforced by `--check-plugin` and by the installer): archive up to 5 MiB, at most 200 files, 20 MiB unpacked, each Lua file at most 1 MiB, only UTF-8 text for Lua, no symbolic links, no absolute or `..` paths, no native libraries or Lua bytecode inside a script plugin.

## Permissions

Ask only for what you use. Each call to a protected function checks that the user approved the permission and raises `permission denied: <name>` otherwise (and writes a `denied` line to the audit log). The user can approve a subset of what you ask for, so handle errors from calls you can live without.

| Permission | What it allows | Risk | Highlighted as dangerous |
|---|---|---|---|
| `note.read` | Read the text, selection, title and tags of the open note | medium | no |
| `note.edit` | Change the open note (replace selection, insert, replace all text) | medium | no |
| `notes.read` | List and read every note in your library | medium | no |
| `notes.write` | Create, overwrite and DELETE notes in your library | high | yes |
| `ui` | Show notifications and ask you questions | low | no |
| `storage` | Keep up to 1 MiB of its own data | low | no |
| `clipboard` | Read and replace your clipboard contents | high | yes |
| `network` | Send and receive data over HTTPS with the hosts it lists | high | yes |
| `theme` | Change theme colours | medium | yes |
| `native` | Run native code with full access to your account (not sandboxed) | critical | yes |
| `notes.index` | Read link, tag and task data about all notes | medium | no |
| `ui.panel` | Show its own panel next to your notes | low | no |
| `editor.complete` | Offer completion suggestions while you type | low | no |
| `editor.links` | Handle clicks on [[links]] in your notes | medium | no |

The last four need `"api": 2`. `notes.index` is read-only but covers every note's links, tags, frontmatter and tasks, so with `network` it can leak the shape of your whole library; `editor.links` lets a plugin decide what happens when you click a `[[link]]`.

Combinations matter more than single permissions: `note.read` + `network` lets a plugin send your note text to the hosts it lists; `notes.read` + `network` can send your whole library. `notes.write` and `note.edit` can destroy text (undo exists, but it is not a backup). Plugins that need no permission at all (typing triggers, commands that only call `hn.time`) are the easiest to trust.

## Events

Subscribe with `hn.on(event, fn)`. A plugin that registered no handler for an event costs nothing when it happens.

```lua test perms=note.read
-- Remember what happened, using only note.read.
local seen = 0

hn.on("app.started", function(version) hn.log("Hyprnotes " .. version .. " is up") end)

hn.on("note.opened", function(path)
  seen = seen + 1
  hn.log("opened " .. path .. " (" .. seen .. " so far)")
end)

hn.on("note.saved", function(path) hn.log("saved " .. path) end)

-- Return a string from note.pre_save to change what is written. 20 ms budget: keep it tiny.
hn.on("note.pre_save", function(text)
  if text:sub(-1) ~= "\n" then return text .. "\n" end
end)
```

Events: `app.started`, `note.opened`, `note.changed` (coalesced to once per 250 ms), `note.saved`, `note.closed`, `selection.changed` (coalesced), `note.pre_save`. Handlers run with a 50 ms budget (`note.pre_save`: 20 ms). Details and arguments are in [plugin-api.md](plugin-api.md#hnonevent-fn).

## Triggers

A trigger replaces text as you type it:

```lua test perms=
hn.trigger{ pattern = "::date", replace = function() return hn.time.format("%Y-%m-%d") end }
hn.trigger{ pattern = "::sig", replace = function() return "Best regards,\nYour Name" end }
```

`pattern` is literal text (2-64 characters), not a Lua pattern. When the text before the cursor ends with it, your `replace` function runs (50 ms budget) and its string replaces what was typed. Pick a prefix people do not type by accident, like `::`.

## Settings

Declare settings with `hn.setting{ id, type, default, title }`; the app renders them on your plugin's card and stores the choice. Read them with `hn.settings.get(id)` every time you need them (the value can change between calls).

```lua test perms=ui
hn.setting{ id = "name", type = "string", default = "friend", title = "Who to greet" }
hn.setting{ id = "excited", type = "bool", default = true, title = "Add an exclamation mark" }

hn.command{ id = "greet", title = "Greet", run = function()
  hn.ui.notify("Hello " .. hn.settings.get("name") .. (hn.settings.get("excited") and "!" or "."))
end }
```

Things that are decided when the plugin **loads** (such as which triggers exist) do not change when the user edits a setting; tell them to press Reload (the `snippets` example does this).

Plugin-private data that is not a setting goes through `hn.storage` (permission `storage`, 1 MiB).

## Plugin API 2: panels, completion and the note index

Set `"api": 2` in `plugin.json` to get four more permissions and the functions that go with them. Nothing changes for `"api": 1` plugins: they keep exactly the surface described above, and the new functions do not exist for them.

```json manifest
{
  "id": "open-tasks",
  "name": "Open Tasks",
  "version": "1.0.0",
  "author": "Your Name",
  "description": "A panel that lists unchecked tasks from all notes.",
  "api": 2,
  "tier": "script",
  "permissions": ["ui.panel", "notes.index", "notes.read"],
  "min_app": "0.1.0"
}
```

- **The note index** (`notes.index`): `hn.notes.links`, `backlinks`, `resolve`, `frontmatter` and `query` give you link, tag and task data without reading every note. `query` takes a *table*, never SQL.
- **Panels** (`ui.panel`): `hn.panel{...}` adds a page to the side dock. You return blocks (headings, text, lists of items, buttons); the app draws them, so a plugin cannot show arbitrary widgets.
- **Completion** (`editor.complete`): `hn.complete{ trigger = "[[", items = ... }` fills a popup while the user types. `items` also gets `ctx.at_line_start` (only whitespace before the trigger on its line), and an item with `markdown = true` is inserted as Markdown in the visual editor, so a `/` menu can offer headings and lists.
- **Link handlers** (`editor.links`): react when the user clicks a `[[link]]`.
- **Open and rename** (`notes.read`, `notes.write`): `hn.notes.open` shows a note, `hn.notes.rename` renames one (opt-in link rewriting; the app asks the user first).

A complete panel in a few lines:

```lua test perms=ui.panel,notes.index,notes.read api=2
hn.panel{
  id = "tasks",
  title = "Open tasks",
  refresh_on = { "note.saved" },
  render = function()
    local rows, err = hn.notes.query{ from = "tasks", where = { { field = "done", op = "=", value = false } }, limit = 50 }
    if not rows then return { { type = "empty", text = "Index not available: " .. tostring(err) } } end
    local items = {}
    for i, row in ipairs(rows) do
      items[i] = { type = "item", title = row.text, subtitle = row.path, on_click = function() hn.notes.open(row.path) end }
    end
    return { { type = "heading", text = #rows .. " open tasks" }, { type = "list", items = items } }
  end,
}
```

Things worth knowing:

- **Laziness is kept.** The app lists your panels, completion triggers and link handlers from a cache of your registrations without starting Lua. Your Lua state is created the first time something is actually asked of it (a render, a completion request, a click). Panels that are not visible are never rendered.
- **Index calls can fail softly.** The index may be busy or not ready: they return `nil, "<reason>"` (for example `"timeout: index call exceeded 100 ms"`). Always handle `nil`; the examples show empty-state blocks.
- **Budgets are tight on purpose.** A panel render has 50 ms and completion 20 ms, because the user is waiting for them. Do the work in the index (`query`), not in loops over `hn.notes.read`.
- **Scaffold.** `createTemplate(dir, name, &err, 2)` writes an API-2 backlinks panel sample (the plain `createTemplate(dir, name, &err)` still writes the API-1 sample).

Full reference: [plugin-api.md](plugin-api.md#the-note-index-hnnotes-api-2), [panels](plugin-api.md#panels-api-2) and [editor hooks](plugin-api.md#editor-hooks-api-2).

## Budgets and limits you will notice

| What | Limit |
|---|---|
| Event handler | 50 ms wall clock (`note.pre_save` 20 ms) |
| Command / toolbar / menu | 2 s |
| Trigger replacement | 50 ms |
| Top-level code when the plugin loads | 250 ms |
| Memory | 16 MiB Lua heap per plugin |
| Strings from/to the app | 4 MiB |
| Pattern matching (`find`, `match`, `gmatch`, `gsub`) | subject 1 MiB, pattern 256 bytes, 24 quantifiers, 4 000 000 matcher steps per call |
| Notifications | 10 per 10 seconds |
| HTTP | 20 requests per minute, 1 MiB response |
| Notes written | 50 per callback |
| Panel render (API 2) | 50 ms, at most 200 blocks |
| Completion (API 2) | 20 ms per plugin, at most 50 items |
| Index calls (API 2) | 100 ms each, 500 ms per callback, 200 per second, 500 rows, strings 64 KiB |
| Notes opened (API 2) | 5 per callback |
| Three failures in a row | the plugin is switched off and you are told |

A callback that overruns its budget is stopped, counts as a failure and its error goes to the log. Time spent waiting for the user in a dialog or for the network does not count.

## Debugging

- **`hn.log(...)`** (and `print`) write to the application log tagged with your plugin id. Start Hyprnotes from a terminal to watch it:

  ```lua test perms=note.read
  hn.command{ id = "debug", title = "Debug dump", run = function()
    hn.log("path=", hn.note.path(), "selection=", #hn.note.selection())
  end }
  ```

- **Errors** show up in the log as `plugin-id: main.lua:12: message` and in the audit log as `failure`. After three consecutive failures the plugin is disabled (`auto-disable` in the audit log); fix it and enable it again.
- **Audit log**: `$XDG_STATE_HOME/hyprnotes/plugins-audit.jsonl` (default `~/.local/state/hyprnotes/plugins-audit.jsonl`), one JSON object per line, rotated at 1 MiB (three files are kept):

  ```json
  {"ts":"2026-10-01T14:05:09.123Z","event":"denied","plugin":"my-plugin","detail":"note.edit"}
  ```

  Events: `install`, `upgrade`, `consent`, `enable`, `disable`, `remove`, `denied` (a call without permission, or a refused URL), `failure`, `auto-disable`, `tamper` (files changed after approval), `reload-trust`, `network` (method and **host only**), `notes.create` / `notes.write` / `notes.delete` / `notes.rename` (note path; rename also shows the new name), and for API 2 a `denied` line when an index call is refused (rate limit, 100 ms overrun, a rejected query). Note text is never written to the audit log.
- **`permission denied: network (host 'x' is not listed in net_hosts)`**: add the host to `net_hosts` and approve again.
- **`no active note in this context`**: `hn.note.*` only works inside commands and note events, not while the file loads.
- **`pattern too complex`**: your pattern backtracks too much for the 4 000 000-step budget; make it more specific (anchor it with `^`, avoid several `.-` in a row).
- Run `hyprnotes --check-plugin DIR` after every edit; it catches syntax errors before you load anything.

## Packing and sharing

`hyprnotes --pack-plugin DIR` produces a `.hnplugin` file: a zip archive of the plugin folder (`plugin.json` at the top level). People install it from the Plugins page or with a folder. Installed plugins live in `$XDG_DATA_HOME/hyprnotes/plugins/<id>/`.

What users see and what you should expect:

- The plugin is **disabled** after install. The approval dialog lists your permissions in plain language and shows the SHA-256 of the package, so people can compare it with the one you publish next to the download.
- The approval is bound to the exact files. Any later change to a file disables the plugin and asks again. Publishing an update with the same content keeps the approval; a changed update asks again, and asking for a new permission always asks again.
- Keep the permission list short, explain each one in your README, and publish the hash.

## Security model and its limits

What the system does:

- **Validation** before anything is installed: manifest schema, id charset, API version, size caps, no symlinks, no `..` or absolute paths, no native libraries or bytecode in a script plugin, valid UTF-8, Lua syntax check.
- **Consent**: disabled until the user approves; the dialog always shows the third-party warning. **Integrity**: SHA-256 over all files recorded at approval; any change disables the plugin.
- **Sandbox**: one Lua state per plugin, created only when the plugin first has something to do. No `io`, `os`, `debug`, `package`, `dofile`, `loadfile`; `require` only reads files inside the plugin folder; no raw sockets or processes. Plugins cannot see each other's data, globals or modified standard libraries.
- **Resource limits**: wall-clock and instruction budgets per callback, 16 MiB heap, call depth cap, string size caps, bounded `string.rep`/pattern matching/`table.sort`/`table.move`, no `__gc` finalizers; an infinite loop, a memory bomb or a pathological pattern is aborted within the budget (measured in the test-suite: loops stop at the budget, pattern bombs in about 15 ms, memory bombs in tens of milliseconds). Three failures in a row switch the plugin off.
- **Network**: only `https`, only the hosts in `net_hosts`, no credentials in URLs, default port only, size/time/rate caps, hosts (not URLs or bodies) in the audit log. The host application must refuse redirects to other hosts.

What it does **not** do, stated plainly:

- The sandbox is **best effort, not a guarantee**. A bug in Lua, in the C++ glue or in a host bridge could break out of it. Do not treat a script plugin as safe because it is sandboxed; treat it as safer than a native one.
- **Native plugins are not sandboxed at all.**
- With `note.read` and `network` a plugin **can send your text to the hosts it lists**; the sandbox cannot tell a legitimate sync from theft. With `note.edit` or `notes.write` a plugin **can destroy note content**; undo and recovery exist but are not a backup.
- The time limit is wall clock and it is enforced between Lua instructions and inside the bounded library functions. A few C library calls cannot be interrupted once started (sorting 100 000 strings takes about 35 ms, copying or uppercasing a 4 MiB string a few ms). They are capped so no single call can stall the app for long, but the guarantee is "tens of milliseconds", not "microseconds". A plugin can also use many small allowed calls to be annoying inside its budget.
- A plugin that is allowed to show dialogs can pester you; one that can set theme tokens can make the app ugly or unreadable.
- Resource limits protect the app, not other programs: there is no CPU priority or network quota beyond the caps above.
- The audit log is local and plain text. It helps you see what happened; it is not tamper-proof.

If in doubt: read `main.lua`. It is a few hundred lines at most.
