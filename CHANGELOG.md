# Release notes

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
