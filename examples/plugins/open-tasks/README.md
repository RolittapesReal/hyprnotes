# Open Tasks

Adds an **Open tasks** panel that lists unchecked Markdown tasks (`- [ ] ...`) from all notes, ordered by note and line. Click a task to open its note. The **Refresh** button re-runs the query; the panel also refreshes after each save while it is visible.

Shows at most 150 tasks (a panel holds at most 200 blocks).

Permissions: `ui.panel` (show the panel), `notes.index` (read task data about all notes), `notes.read` (open a note when you click a task). No network, nothing is changed in your notes.
