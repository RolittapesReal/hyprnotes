# Theme and config reference

## Themes

Built-ins, in picker order: `modernist` (light and dark), `catppuccin-mocha`, `tokyo-night`, `dracula`, `nord`,
`gruvbox-dark`, and `one-dark`. The six named presets provide dark colors; explicitly selecting Light uses all Modernist light tokens.
Picking a bundled dark preset in Settings switches the color scheme to Dark when the UI is currently light; importing a theme never changes the scheme. Built-ins are compiled into the app and need no configuration files.
Custom themes live at `$XDG_CONFIG_HOME/hyprnotes/themes/<name>.json`
(default `~/.config/hyprnotes/themes/`). Select one with `"theme": "<name>"` in `config.json`.
A sample is at `packaging/share/themes/modernist-example.json`.

```json
{ "version": 1, "base": "modernist", "light": { "accent": "#1F4FB5" }, "dark": { } }
```

- `version` must be `1`. `base` is optional and must be `modernist`.
- `light` / `dark` hold tokens; any omitted token is inherited from the base.
- An invalid file (bad JSON, wrong version, bad color, out-of-range number, unknown base) is rejected as a whole:
  `loadTheme` returns the built-in base and `hn::theme::lastThemeError()` describes why. Unknown keys are ignored.

### Import and export

Settings > Appearance > Theme: pick a theme (applies live and is saved), **Import theme...**, **Export current theme...**, **Remove**
(user themes only; built-ins cannot be removed). Dropping a `.json`/`.yaml`/`.yml` file on the Settings dialog or the organizer
(outside the text editor) imports and selects it. CLI: `hyprnotes --import-theme FILE` imports the file and, when `config.json`
exists, selects it; a running instance applies it live through its config watcher. The result is printed; exit code 1 on failure.

Accepted files: at most 64 KiB; (1) a Hyprnotes theme (`"version": 1`, optional `"name"`), (2) base16 YAML (all of `base00`-`base0F`),
(3) a VS Code color theme JSON (needs `editor.background` and `editor.foreground`; comments in the JSON are not supported).
Foreign themes become a Hyprnotes theme whose single section (`dark` or `light`) is chosen by the luminance of `base00` /
`editor.background`; the other scheme inherits modernist. base16 mapping: bg=00, surface=01, selection=02, border=03, muted=04, text=05,
danger=08, success=0B, accent=0D, noteAccent=08,0D,0A,0B,03,07. VS Code: bg/text, `editor.selectionBackground`, `sideBar.background`,
`panel.border`, `editorLineNumber.foreground`, `errorForeground`, accent from `focusBorder` or `button.background`.

Rules: the installed name is the `name` field (or file name) reduced to `[A-Za-z0-9._-]`, max 48; names with `/`, `\` or `..` are rejected;
all seven built-in IDs are reserved, case-insensitively. Files using those IDs are hidden from the picker and cannot override a built-in;
they are left on disk. An identical theme is reused; a different theme with an existing name is installed as `name-2`, `name-3`, ...
(never overwritten). A text/bg contrast below 4.5:1 is imported with a warning. Export writes a user theme unchanged, or exports a built-in
as `<canonical-id>-copy` (for example, `nord-copy`). Import that copy to edit or remove it normally; dark-only copies retain Modernist light inheritance.

### Tokens

| token | type | modernist light / dark |
|---|---|---|
| bg | `#rrggbb` | #F3F1EC / #111111 |
| surface | color | #FBFAF7 / #1A1A1A |
| text | color | #111111 / #F3F1EC |
| muted | color | #5C5A55 / #A3A09A |
| accent | color | #D92E18 / #FF5A43 (vermilion) |
| accentText | color, text on accent | #FFFFFF / #111111 |
| border | color | #C9C5BB / #3A3A38 |
| danger, success, selection | color | see `src/theme/theme.cpp` |
| noteAccent | array of exactly 6 colors (red, blue, yellow, green, black, white-ish) | |
| fontFamily | string (default Inter, then Noto Sans, then sans-serif) | |
| monoFamily | string (default JetBrains Mono, then monospace) | |
| baseSize | int px 8..32 | 14 |
| lineHeight | number 1.0..3.0 | 1.45 |
| padding | int px 0..64 | 16 (editor padding; aligned with organizer header text) |
| radius | int px 0..32 | 4 |
| borderWidth | int px 0..4 | 1 |

Modernist contrast (WCAG, checked by `theme_test`): text/bg, muted/bg, accentText/accent all >= 4.5:1 in both schemes.
The light accent is #D92E18, slightly darker than #E5341D, because white on #E5341D is only 4.34:1.

The look is flat: no gradients, shadows or blur; 8px grid; 2px accent focus rings; selected rows show a 3px accent bar at left.
Built-ins use 4px control corners and 6px popup corners. Custom popup radius is control radius + 2px, capped at 32px;
an explicit `radius: 0` keeps both square. Document surfaces and structural panes remain square.
QSS cannot do letter-spacing or uppercase, so use `hn::theme::labelFont(theme)` for tracked uppercase labels.

## config.json

`$XDG_CONFIG_HOME/hyprnotes/config.json`, never written at load, only by `Config::save()` (atomic).

```json
{
  "version": 1,
  "theme": "modernist",
  "colorScheme": "system",
  "notesFolder": "/home/me/.local/share/hyprnotes/notes",
  "trayEnabled": false,
  "reduceMotion": false,
  "keybindings": { "bold": "Ctrl+B", "new-note": "Ctrl+N" },
  "toolbar": ["bold", "italic", "link"],
  "windows": { "stickySize": [360, 300], "stickyMin": [260, 180], "organizerSize": [900, 640], "organizerFloating": false }
}
```

| field | validation |
|---|---|
| theme | plain name, no path separators |
| colorScheme | `system`, `light` or `dark` |
| notesFolder | absolute path |
| trayEnabled, reduceMotion | boolean |
| keybindings | object action -> Qt portable key sequence; `""` unbinds; missing actions keep defaults |
| toolbar | unique, non-empty action ids, in order |
| windows.* | `[w,h]` integers (sticky >= 100, organizer >= 200) |

Bad file (malformed JSON, wrong `version`): all previous settings kept, `Config::lastError()` set.
Bad field: that field keeps its previous value, other fields apply, `lastError()` lists the problems.
`Config::watch()` watches the config directory (and nearest existing ancestor if it is missing), coalesces events (50 ms),
re-arms after atomic replaces and emits `changed()` only when settings or the error state actually changed.

## AnimationPolicy

`hn::theme::AnimationPolicy{reduceMotion}.duration(ms)` returns `clamp(ms, 100, 150)`, or 0 when reduce motion is set.
