# Theme and config reference

## Themes

The picker lists `modernist` (light and dark), `catppuccin-mocha`, `tokyo-night`, `dracula`, `nord`,
`gruvbox-dark` and `one-dark`, in that order. The six named presets define dark colors. Selecting Light uses the Modernist light tokens.
If the UI is in Light mode, choosing a bundled dark preset in Settings switches the color scheme to Dark. Importing a theme does not change the scheme. Built-ins are compiled into the app and need no configuration files.
Custom themes are stored at `$XDG_CONFIG_HOME/hyprnotes/themes/<name>.json`
(default `~/.config/hyprnotes/themes/`). Set `"theme": "<name>"` in `config.json` to select one.
A sample is at `packaging/share/themes/modernist-example.json`.

```json
{ "version": 1, "base": "modernist", "light": { "accent": "#1F4FB5" }, "dark": { } }
```

- `version` must be `1`. `base` is optional and must be `modernist`.
- `light` and `dark` hold tokens. Omitted tokens inherit from the base.
- An invalid file (malformed JSON, wrong version, invalid color, out-of-range number or unknown base) is rejected as a whole. `loadTheme` returns the built-in base, and `hn::theme::lastThemeError()` reports the reason. Unknown keys are ignored.

### Import and export

In Settings > Appearance > Theme, choose a theme to apply and save it, or use **Import theme...**, **Export current theme...** or **Remove**. Remove is available for user themes only. Drop a `.json`, `.yaml` or `.yml` file on the Settings dialog or on the organizer outside the text editor to import and select it. From the command line, `hyprnotes --import-theme FILE` imports the file and selects it if `config.json` exists. A running instance applies the change through its config watcher. The command prints the result and exits with code 1 on failure.

Accepted files are at most 64 KiB and use one of these formats: a Hyprnotes theme (`"version": 1`, optional `"name"`); base16 YAML with all values from `base00` through `base0F`; or a VS Code color theme JSON with `editor.background` and `editor.foreground`. Comments are not supported in JSON.
Imported base16 and VS Code themes become Hyprnotes themes with one section, `dark` or `light`, chosen from the luminance of `base00` or `editor.background`. The other scheme inherits Modernist. base16 mapping: bg=00, surface=01, selection=02, border=03, muted=04, text=05,
danger=08, success=0B, accent=0D, noteAccent=08,0D,0A,0B,03,07. VS Code: bg/text, `editor.selectionBackground`, `sideBar.background`,
`panel.border`, `editorLineNumber.foreground`, `errorForeground`, accent from `focusBorder` or `button.background`.

The installed name comes from the `name` field or file name and is reduced to `[A-Za-z0-9._-]`, with a 48-character limit. Names containing `/`, `\` or `..` are rejected;
all seven built-in IDs are reserved, case-insensitively. Files with those IDs stay on disk but are hidden from the picker and cannot override a built-in. An identical theme is reused. If a different theme already uses that name, the new one is saved as `name-2`, `name-3` and so on; existing themes are never overwritten. A text/background contrast ratio below 4.5:1 produces a warning during import. Export writes a user theme unchanged. A built-in is exported as `<canonical-id>-copy` (for example, `nord-copy`). Import that copy to edit or remove it; a dark-only copy keeps its Modernist light inheritance.

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

Modernist contrast meets WCAG 4.5:1 for text/bg, muted/bg and accentText/accent in both schemes, as checked by `theme_test`.
The light accent is #D92E18, slightly darker than #E5341D, because white on #E5341D is only 4.34:1.

The interface uses no gradients, shadows or blur. It follows an 8px grid, uses 2px accent focus rings, and marks selected rows with a 3px accent bar on the left.
Built-in controls have 4px corners and popups have 6px corners. For custom themes, popup radius is the control radius plus 2px, capped at 32px. Setting `radius: 0` keeps both square. Document surfaces and structural panes are always square.
QSS cannot do letter-spacing or uppercase, so use `hn::theme::labelFont(theme)` for tracked uppercase labels.

## config.json

`config.json` is stored at `$XDG_CONFIG_HOME/hyprnotes/config.json`. Loading it does not write the file; `Config::save()` writes it atomically.

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

If the file is malformed or has the wrong `version`, all previous settings are kept and `Config::lastError()` is set.
If a field is invalid, that field keeps its previous value while valid fields are applied. `lastError()` lists the problems.
`Config::watch()` watches the config directory, or its nearest existing ancestor if the directory is missing. It coalesces events for 50 ms, re-arms after atomic replacements, and emits `changed()` only when settings or error state changes.

## AnimationPolicy

`hn::theme::AnimationPolicy{reduceMotion}.duration(ms)` returns `clamp(ms, 100, 150)`, or 0 when reduce motion is set.
