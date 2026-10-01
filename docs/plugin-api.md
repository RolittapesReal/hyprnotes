# Hyprnotes plugin API reference (API version 1)

This is the reference for the `hn` table that script plugins (Lua 5.5, sandboxed) receive. For a guided introduction read [plugins.md](plugins.md).
It is checked against the implementation: an automated test (`plugins_docs_test`) lists every function the sandbox exposes and fails if one is missing here (or documented but missing there), and it **runs every `lua` code block below** as a real plugin against fake bridges.

Conventions used in every entry:

- **Permission**: the entry in `plugin.json` `"permissions"` that the user must have approved. Calling without it raises `permission denied: <name>` (and writes a `denied` line to the audit log).
- **Budget**: wall-clock limit of the callback the call runs in (events 50 ms, commands 2 s, `note.pre_save` 20 ms, triggers 50 ms, top-level load 250 ms). Time spent *waiting* for the user in `hn.ui.prompt/confirm/pick` and for the network does not count.
- **Errors**: Lua errors you can catch with `pcall`. A callback that raises (or overruns its budget) counts as a failure; three failures in a row disable the plugin.
- Strings crossing the plugin/host boundary are UTF-8 and at most 4 MiB.

Each block is a complete plugin `main.lua`. The first line of its fence says which permissions the test grants it.

## Registration

Registration functions are normally called while `main.lua` loads (the top level). Limits: 64 commands, 64 toolbar buttons, 64 menu items, 64 settings, 64 triggers and 64 handlers per event, per plugin.

### hn.command(spec)

`hn.command{ id, title, run, key? }`  -  adds a command to the command palette.

- `id`: 1-64 characters from `a-z 0-9 . _ -`, unique among your commands. The app addresses it as `<plugin-id>:<id>`.
- `title`: shown to the user, up to 120 characters.
- `run`: function called with no arguments.
- `key`: optional suggested shortcut text, up to 32 characters (the user decides whether to bind it).

Permission: none. Budget of `run`: 2 s. Errors: `a table argument is required`, `invalid id '...'`, `field 'title' must be a string`, `field 'run' must be a function`, `duplicate command id or too many commands`.

```lua test perms=ui
hn.command{
  id = "hello",
  title = "Say hello",
  key = "Ctrl+Alt+H",
  run = function() hn.ui.notify("Hello from a command") end,
}
```

### hn.toolbar_button(spec)

`hn.toolbar_button{ id, title, run, icon? }`  -  adds a button to the editor toolbar. `icon` is a short icon name (up to 64 characters); unknown names fall back to the title text.

Permission: none. Budget: 2 s. Errors: as `hn.command` (`duplicate toolbar button id or too many buttons`).

```lua test perms=ui
hn.toolbar_button{ id = "ping", title = "Ping", icon = "bell", run = function() hn.ui.notify("pong") end }
```

### hn.menu_item(spec)

`hn.menu_item{ id, title, run, where? }`  -  adds a menu entry. `where` is `"tools"` (default) or `"note"` (the note context menu).

Permission: none. Budget: 2 s. Errors: as `hn.command`, plus `where must be "note" or "tools"`.

```lua test perms=ui
hn.menu_item{ id = "about", title = "About this plugin", where = "tools", run = function() hn.ui.notify("v" .. hn.version) end }
hn.menu_item{ id = "ctx", title = "Context action", where = "note", run = function() hn.ui.notify("context") end }
```

### hn.on(event, fn)

Subscribes `fn` to an event. The function receives one string argument. You may register several handlers for one event (up to 64).

| Event | Argument | Notes |
|---|---|---|
| `app.started` | app version | once, after the app is up |
| `note.opened` | note path | a note became the active note |
| `note.changed` | note path | coalesced: at most once per 250 ms |
| `note.saved` | note path | after a successful save |
| `note.closed` | note path | |
| `selection.changed` | note path | coalesced like `note.changed` |
| `note.pre_save` | the note text | may **return** a replacement text; budget 20 ms; any non-string result is ignored |

Handlers run with a 50 ms budget (20 ms for `note.pre_save`). The active note is available through `hn.note.*` inside note events; `app.started` has no active note. The arguments above are what the runtime hands to your function; which note events the app emits when is up to the app.

Permission: none to subscribe (the calls inside need their own). Errors: `unknown event '...'`, `bad argument #2 (function expected)`, `too many handlers for <event>`.

```lua test perms=note.read
hn.on("note.saved", function(path)
  hn.log("saved " .. path .. " (" .. #hn.note.text() .. " bytes)")
end)

-- note.pre_save: return the text to be saved. Here: strip trailing spaces from every line.
hn.on("note.pre_save", function(text)
  return (text:gsub("[ \t]+\n", "\n"))
end)
```

### hn.trigger(spec)

`hn.trigger{ pattern, replace }`  -  typing triggers. When the text before the cursor ends with `pattern`, the app calls `replace(pattern)` and substitutes the string it returns.

- `pattern` is **literal text** (not a Lua pattern), 2 to 64 characters, unique within the plugin. The longest matching pattern wins across all plugins.
- `replace` must return a string (up to 4 MiB); anything else cancels the replacement. Budget: 50 ms.

Permission: none. Errors: `trigger pattern must be 2-64 characters and unique`, `field 'replace' must be a function`.

```lua test perms=
hn.trigger{ pattern = "::now", replace = function() return hn.time.format("%Y-%m-%d %H:%M") end }
hn.trigger{ pattern = "::shrug", replace = function() return "shrug" end }
```

### hn.setting(spec)

`hn.setting{ id, type, default, title }`  -  declares a user-editable setting, rendered on the plugin's card. `type` is `"bool"`, `"string"` or `"number"` and `default` must have that type. Read the current value with [`hn.settings.get`](#hnsettingsgetid). Setting ids follow the same rules as command ids; `title` is up to 120 characters.

Permission: none. Errors: `type must be "bool", "string" or "number" and 'default' must match it`, `duplicate setting id`, `too many settings`.

```lua test perms=ui
hn.setting{ id = "greeting", type = "string", default = "Hello", title = "Greeting text" }
hn.setting{ id = "repeat", type = "number", default = 2, title = "Repeat count" }
hn.setting{ id = "loud", type = "bool", default = false, title = "Shout" }

hn.command{ id = "greet", title = "Greet", run = function()
  local text = hn.settings.get("greeting"):rep(hn.settings.get("repeat"), " ")
  if hn.settings.get("loud") then text = text:upper() end
  hn.ui.notify(text)
end }
```

### hn.settings.get(id)

Returns the current value of a setting (what the user chose, else its default), or `nil` for an unknown id. Permission: none. Budget: trivial.

```lua test perms=ui
hn.setting{ id = "who", type = "string", default = "world", title = "Who" }
hn.command{ id = "hi", title = "Hi", run = function()
  hn.ui.notify("Hello " .. hn.settings.get("who"))
  assert(hn.settings.get("nope") == nil)
end }
```

## Logging and information

### hn.log(...)

Writes its arguments (converted with `tostring`, joined by tabs) to the application log, tagged with your plugin id. `print` is an alias. Each callback may log 200 lines, each cut at 2000 characters; further lines are dropped silently. Permission: none.

```lua test perms=
hn.log("plugin loaded")
print("print goes to the same place", 42, true, nil)
```

### hn.version

A string: the Hyprnotes version (for example `"0.1.0"`). Use it to adapt to newer apps; the plugin API version itself is fixed by the `"api"` field in `plugin.json`.

```lua test perms=
hn.command{ id = "v", title = "Version", run = function() hn.log("running on Hyprnotes " .. hn.version) end }
```

### hn.time.now()

Returns the current time as integer seconds since 1970-01-01 UTC. Permission: none. (Scripts have no `os` library; this and `hn.time.format` replace `os.time`/`os.date`.)

```lua test perms=
hn.command{ id = "t", title = "Now", run = function() assert(math.type(hn.time.now()) == "integer") end }
```

### hn.time.format(fmt, epoch?, utc?)

Formats a time with C `strftime` codes (`%Y-%m-%d`, `%H:%M`, `%A`, `%B` ...). `epoch` defaults to now, `utc` (boolean) selects UTC instead of the local time zone. The format is printable ASCII up to 64 characters and the result at most 255 bytes. Permission: none.

Errors: `format produces no output or is too long`, `format must be printable ASCII`, `bad argument #2 (integer epoch seconds expected)`, `epoch out of range (years 1-9999)`.

```lua test perms=
hn.command{ id = "fmt", title = "Format", run = function()
  assert(hn.time.format("%Y-%m-%d %H:%M:%S", 0, true) == "1970-01-01 00:00:00")
  assert(#hn.time.format("%A %d %B %Y") > 8)
  assert(not pcall(hn.time.format, "%Y", 1e15))
end }
```

## The open note: `hn.note`

These work on the note in the active window. Outside a note context (while `main.lua` loads, in `app.started`) they raise `no active note in this context`. All edits made during one callback form **one undo step**.

### hn.note.text()

Returns the whole text of the open note. Permission: `note.read`. Errors: `no active note in this context`, `value too large for the plugin memory limit` (a note of several MiB on a plugin that already holds a lot of memory).

```lua test perms=note.read,ui
hn.command{ id = "len", title = "Note length", run = function() hn.ui.notify(#hn.note.text() .. " bytes") end }
```

### hn.note.selection()

Returns the selected text, or `""` when nothing is selected. Permission: `note.read`.

```lua test perms=note.read,ui
hn.command{ id = "sel", title = "Selection", run = function()
  local s = hn.note.selection()
  hn.ui.notify(s == "" and "nothing selected" or ("selected: " .. s))
end }
```

### hn.note.path()

Returns the note's path relative to the library (for example `"projects/plan.md"`). Permission: `note.read`.

```lua test perms=note.read
hn.command{ id = "path", title = "Path", run = function() hn.log(hn.note.path()) end }
```

### hn.note.title()

Returns the note's title. Permission: `note.read`.

```lua test perms=note.read
hn.command{ id = "title", title = "Title", run = function() hn.log(hn.note.title()) end }
```

### hn.note.tags()

Returns an array of the note's tags (strings, possibly empty). Permission: `note.read`.

```lua test perms=note.read
hn.command{ id = "tags", title = "Tags", run = function() hn.log(#hn.note.tags() .. " tags: " .. table.concat(hn.note.tags(), ", ")) end }
```

### hn.note.replace_selection(s)

Replaces the selection with `s` (inserts at the cursor when nothing is selected). Permission: `note.edit`. Errors: `bad argument #1 (string expected)`, `bad argument #1 (string too large, max 4194304 bytes)`.

```lua test perms=note.read,note.edit
hn.command{ id = "up", title = "Uppercase selection", run = function() hn.note.replace_selection(hn.note.selection():upper()) end }
```

### hn.note.insert(s)

Inserts `s` at the cursor. Permission: `note.edit`.

```lua test perms=note.edit
hn.command{ id = "sig", title = "Insert signature", run = function() hn.note.insert("\n-- sent from Hyprnotes\n") end }
```

### hn.note.set_text(s)

Replaces the entire note text. This is the most destructive call in the API: with `note.edit` a plugin can erase a note (undo restores it, but undo is not a backup). Permission: `note.edit`.

```lua test perms=note.read,note.edit
hn.command{ id = "norm", title = "Normalise line endings", run = function()
  hn.note.set_text((hn.note.text():gsub("\r\n", "\n")))
end }
```

## The library: `hn.notes`

Paths are relative to the notes library: at most 512 bytes, no leading `/`, no `\`, no `..` segment, otherwise `invalid note path`.

### hn.notes.list(query?)

Returns an array of `{ path = ..., title = ... }` for notes matching `query` (up to 256 characters; empty or `nil` lists everything), at most 1000 entries. Permission: `notes.read`.

```lua test perms=notes.read,ui
hn.command{ id = "ls", title = "List notes", run = function()
  local items = hn.notes.list("")
  hn.ui.notify(#items .. " notes, first: " .. items[1].title .. " (" .. items[1].path .. ")")
end }
```

### hn.notes.read(path)

Returns the note text, or `nil, "not found"`. Permission: `notes.read`. Errors: `invalid note path`.

```lua test perms=notes.read,ui
hn.command{ id = "rd", title = "Read a note", run = function()
  local text, err = hn.notes.read("inbox/todo.md")
  hn.ui.notify(text and ("read " .. #text .. " bytes") or ("failed: " .. err))
end }
```

### hn.notes.create(title, text)

Creates a note and returns its path, or `nil, "create failed"`. The app chooses the file name from the title. Permission: `notes.write` (dangerous). Writes are audited; at most 50 `notes.create/write/delete` calls per callback (`too many note writes in one callback (max 50)`).

```lua test perms=notes.write,ui
hn.command{ id = "mk", title = "New scratch note", run = function()
  local path = hn.notes.create("Scratch", "# Scratch\n")
  hn.ui.notify("created " .. tostring(path))
end }
```

### hn.notes.write(path, text)

Creates or **overwrites** the note at `path` and returns `true` on success. Permission: `notes.write` (dangerous), audited.

```lua test perms=notes.write
hn.command{ id = "wr", title = "Write a note", run = function()
  assert(hn.notes.write("daily/today.md", "# Today\n"))
end }
```

### hn.notes.delete(path)

Deletes the note and returns `true` on success. Permission: `notes.write` (dangerous), audited. Whether the note goes to a trash folder is up to the app; do not rely on it.

```lua test perms=notes.write,ui
hn.command{ id = "rm", title = "Delete scratch", run = function()
  if hn.ui.confirm("Delete scratch/tmp.md?") then hn.notes.delete("scratch/tmp.md") end
end }
```

## Dialogs: `hn.ui`

All need permission `ui`. The blocking calls (`prompt`, `confirm`, `pick`) show a dialog and wait; the waiting time is **not** charged to your callback budget, but the app stays busy until the user answers, so use them sparingly. Calling one from a `note.changed` handler is a good way to annoy people.

### hn.ui.notify(message)

Shows a short notification (the first 500 characters; the argument may be up to 4096). Returns `true`, or `false` if the call was dropped by the rate limit of 10 notifications per 10 seconds. Never blocks.

```lua test perms=ui
hn.command{ id = "n", title = "Notify", run = function() assert(hn.ui.notify("Saved a copy")) end }
```

### hn.ui.prompt(title, label, default?)

Asks for a line of text. Returns the string, or `nil` when the user cancels. Limits: title 200, label 400, default 4096 characters.

```lua test perms=ui
hn.command{ id = "ask", title = "Ask", run = function()
  local name = hn.ui.prompt("Greeting", "Your name?", "friend")
  if name then hn.ui.notify("Hello " .. name) end
end }
```

### hn.ui.confirm(message)

Asks a yes/no question (message up to 1000 characters) and returns `true` or `false`.

```lua test perms=ui
hn.command{ id = "sure", title = "Confirm", run = function()
  if hn.ui.confirm("Really do it?") then hn.ui.notify("done") end
end }
```

### hn.ui.pick(title, items)

Shows a list and returns the **1-based index** of the chosen item, or `nil` if cancelled. `items` is an array of up to 200 strings of up to 400 characters each.

```lua test perms=ui
hn.command{ id = "choose", title = "Choose", run = function()
  local items = { "red", "green", "blue" }
  local i = hn.ui.pick("Colour", items)
  if i then hn.ui.notify("picked " .. items[i]) end
end }
```

## Storage and settings

### hn.storage.get(key)

Returns the value stored under `key` (a string up to 128 bytes), or `nil`. Permission: `storage`. Each plugin has its own store (`plugin-data/<id>.json` in the state directory); other plugins cannot see it.

### hn.storage.set(key, value)

Stores a value. Values are `nil` (deletes the key), booleans, numbers (no NaN or infinity), strings, and tables that are either pure arrays or have only string keys, nested at most 16 deep with at most 200 000 elements. Functions, userdata and cyclic tables are rejected. The whole store may hold 1 MiB of JSON (`storage quota exceeded (1 MiB per plugin)`). Permission: `storage`. Errors: `cannot store a function`, `table keys must be strings or array indices`, `value is nested too deeply`.

```lua test perms=storage,ui
hn.command{ id = "count", title = "Count runs", run = function()
  local n = (hn.storage.get("runs") or 0) + 1
  hn.storage.set("runs", n)
  hn.storage.set("last", { when = hn.time.now(), tags = { "a", "b" } })
  hn.ui.notify("run number " .. n)
end }
```

## JSON: `hn.json`

No permission. Useful together with `hn.http` and `hn.storage`.

### hn.json.encode(value)

Returns compact JSON text for a value that [`hn.storage.set`](#hnstoragesetkey-value) would accept. Errors as for storage values.

### hn.json.decode(text)

Parses JSON text (up to 4 MiB) and returns the Lua value, or `nil, "invalid JSON"`. Whole-number values become Lua integers, `null` becomes `nil`.

```lua test perms=
hn.command{ id = "json", title = "JSON round trip", run = function()
  local text = hn.json.encode({ name = "note", tags = { "a", "b" }, n = 3 })
  local back = hn.json.decode(text)
  assert(back.name == "note" and back.tags[2] == "b" and math.type(back.n) == "integer")
  local bad, err = hn.json.decode("{oops")
  assert(bad == nil and err == "invalid JSON")
end }
```

## Clipboard

### hn.clipboard.get()

Returns the clipboard text (cut at 1 MiB). Permission: `clipboard` (dangerous: the clipboard often holds passwords).

### hn.clipboard.set(s)

Replaces the clipboard text. Permission: `clipboard` (dangerous).

```lua test perms=clipboard,note.read,ui
hn.command{ id = "copy", title = "Copy selection, show clipboard", run = function()
  hn.clipboard.set(hn.note.selection())
  hn.ui.notify("clipboard: " .. hn.clipboard.get())
end }
```

## Network: `hn.http`

Permission `network` (dangerous) **and** the host must be listed in `plugin.json` `"net_hosts"`. The URL must be `https://`, with no user name or password, on the default port, and the host must match a `net_hosts` entry exactly. Redirects to any other host (or to `http`) must be refused by the host application. Limits: URL 2048 characters, request body 256 KiB, response body 1 MiB, 10 s timeout, 20 requests per minute per plugin (over the limit the call returns `nil, "rate limit exceeded"`), at most 8 request headers (names `A-Za-z0-9-` up to 40 characters, values up to 512 without line breaks; `Host`, `Content-Length`, `Connection`, `Transfer-Encoding` and `Cookie` are refused). Time spent waiting does not count against the callback budget. The audit log records the host only, never the URL path or the body.

Both functions return a table `{ status = <number>, body = <string>, ok = <true for 2xx> }` or `nil, "<error message>"` (timeout, connection error, response too large).

### hn.http.get(url, options?)

`options` may contain `headers = { ["Accept"] = "application/json" }`.

### hn.http.post(url, body, options?)

`body` is a string up to 256 KiB.

Errors (raised): `permission denied: network`, `permission denied: network (only https:// URLs are allowed)`, `permission denied: network (host 'x' is not listed in net_hosts)`, `header 'X' is not allowed`.

```lua test perms=network,note.read,ui hosts=api.example.com
-- plugin.json needs:  "permissions": ["network", ...],  "net_hosts": ["api.example.com"]
hn.command{ id = "fetch", title = "Fetch status", run = function()
  local r, err = hn.http.get("https://api.example.com/status", { headers = { Accept = "application/json" } })
  if not r then hn.ui.notify("network error: " .. err) return end
  hn.ui.notify("status " .. r.status .. (r.ok and " (ok)" or ""))
  local posted = hn.http.post("https://api.example.com/log", hn.json.encode({ title = hn.note.title() }))
  assert(posted)
end }
```

## Theme

### hn.theme.set_token(token, value)

Sets one theme colour/value token for the running session and returns `true` if the app accepted it. `token` matches `^[a-z0-9][a-z0-9._-]{0,63}$`, `value` is up to 64 characters (for example `"#2a6cff"`). Permission: `theme` (dangerous: a plugin can make the app unreadable). The token names are those of the theme reference (`docs/theme-reference.md`).

```lua test perms=theme
hn.command{ id = "accent", title = "Blue accent", run = function() hn.theme.set_token("accent", "#2a6cff") end }
```

## The sandbox and the standard library

You get `string`, `table`, `math`, `utf8`, `coroutine` and the base functions, with these differences.

| Item | Behaviour |
|---|---|
| `io`, `os`, `debug`, `package`, `dofile`, `loadfile`, `warn`, `string.dump` | do not exist (`nil`). Use `hn.time` instead of `os.time`/`os.date`. |
| `print` | goes to `hn.log` |
| `require(name)` | loads `name.lua` (dots become folders) **from your own plugin folder only**, runs it once and caches the result. Anything else raises `module '...' not found in the plugin folder`. |
| `load(text [, chunkname [, mode [, env]]])` | text chunks only (at most 1 MiB); binary chunks are refused |
| `collectgarbage` | only `"collect"` and `"count"` |
| `setmetatable` | refuses a metatable that has `__gc` (finalizers cannot be interrupted) |
| `string.rep` | result at most 4 MiB |
| `string.find/match/gmatch/gsub` | subject at most 1 MiB, pattern at most 256 bytes and 24 quantifiers (`* + - ?`); each call has a 4 000 000-step matcher budget and also stops at your callback's wall-clock limit. Exceeding it raises `pattern too complex (step budget exceeded)`. Matching rules are exactly Lua's. |
| `table.sort` | tables of at most 100 000 elements (a default-order sort of that many strings takes about 35 ms in one uninterruptible C call; a comparison function you pass is interrupted normally) |
| `table.move` | at most 262 144 elements per call |
| string comparison (`<`, `table.sort` default order) | follows the process locale (stock Lua behaviour), so `"a" < "B"` can differ between machines. Compare `s:lower()` or bytes if you need determinism. |

Per-plugin limits: 16 MiB Lua heap (script code can fill it to 15.75 MiB; the last 256 KiB is reserved so host functions never run out of memory half way), call depth 400, a time budget per callback and an instruction budget behind it. `__gc` is rejected, coroutines share the callback's budget, and no plugin can see another plugin's globals, metatables or standard library tables.
