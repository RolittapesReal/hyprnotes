# Daily Note

Run **Create or find today's note** to make `daily/YYYY-MM-DD.md` from a template. If the note exists it is left alone and you get a notification with its path.

Settings: folder (default `daily`) and the template text.

Note: plugin API 1 can create notes but cannot switch the editor to a note, so open the note from the library afterwards.

Permissions: `notes.read`, `notes.write` (dangerous: a plugin with this permission can overwrite and delete notes), `ui`.
