# Backlinks Panel

Adds a **Backlinks** panel to the side dock. For the open note it lists every note that links to it (`[[wiki links]]`, aliases and headings included) together with the sentence around the link. Click an entry to open that note.

Setting: *Most backlinks to show* (default 50, at most 150).

The panel is described, not drawn: the plugin returns headings, a list and items, and Hyprnotes draws them. It re-renders when you open or save a note, only while the panel is visible.

Permissions: `ui.panel` (show the panel), `note.read` (know which note is open), `notes.index` (read the link index), `notes.read` (open a note when you click an entry). No network, nothing is changed in your notes.
