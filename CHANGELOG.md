# Release notes

## 0.2.2 — in preparation

- Restore the coloured fills in the note-colour picker.
- Match tooltips to the theme’s menu surface, text and border colours.
- Dissolve the previous view over 120 ms when switching notes or settings pages. Navigation stays responsive; Reduce motion disables the transition.
- Use one red-and-white app and tray icon in every theme and colour scheme.
- Give sticky windows a 32 px header, shorter workspace labels, clearer formatting hints and a subtle active-window outline.
- Simplify the No matches state and add Clear filters. Repeated clicks while results are loading keep clearing filters rather than creating a note.
- Remove the separate shortcuts sheet. F1 opens Settings → Keys, which now also includes fixed shortcuts and Markdown typing tips. Plugin bindings remain editable there.

Notes, settings formats and plugin API versions are unchanged. Version 0.2.2 has not been tagged or published as a release package. See [testing and validation](docs/testing.md) for the checks and their limits.

## 0.2.1 — in preparation

- Fix Markdown import and export for lists, nested quotes, emphasis and whitespace. List structure now survives saving and reopening a note.
- Fix Organizer pagination during index sync so loaded pages remain a unique prefix of the current results.
- Wait for queued Worker task captures to be released before returning to blocking callers.
- Require SQLite 3.35 or newer for search and show an error when the index is unavailable.
- Improve plugin lifecycle handling, permission and trust checks, completion, and installation feedback.
- Improve dialog keyboard access, accessible names, theme contrast, tray rendering and desktop IPC lifecycle handling.
- Add fuzz regressions, sanitizer controls, static-analysis tools, stress tests and an isolated Hyprland test harness.

Plugin API versions 1 and 2 remain supported. Notes remain plain Markdown files.

Focused checks for the Markdown and pagination fixes are complete. Broader qualification and the original performance and memory targets remain open; recorded failures have not been reclassified as passes. See [testing and validation](docs/testing.md) for the runs and their limits. Version 0.2.1 has not been tagged or published.
