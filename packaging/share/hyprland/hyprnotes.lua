-- Hyprnotes integration for Hyprland with a Lua config (verified on 0.56.2).
-- Include from hyprland.lua:   dofile(os.getenv("HOME") .. "/.config/hypr/hyprnotes.lua")
-- Only app-specific rules: nothing here touches global animations, focus or other binds.
--
-- Windows are identified before mapping by their INITIAL title, "hyprnotes-<role>:<token>"
-- (Qt cannot set a per-window app-id). Later title changes do not affect these rules.

-- Sticky notes: floating from the very first frame (no tiled flash), fixed default size.
hl.window_rule({
  name  = "hyprnotes-sticky",
  match = { initial_title = "^hyprnotes-sticky:" },
  float = true,
  size  = "360 300",
})

-- Organizer: a normal centered window. Delete this rule to tile it instead.
hl.window_rule({
  name   = "hyprnotes-organizer",
  match  = { initial_title = "^hyprnotes-organizer:" },
  float  = true,
  size   = "960 640",
  center = true,
})

-- Example bindings: activate the running instance (never start a second one).
-- Change the keys to taste; SUPER+N / SUPER+SHIFT+N may already be used in your config.
hl.bind("SUPER + N",         hl.dsp.exec_cmd("hyprnotes --new-note"),       { description = "Hyprnotes: new note" })
hl.bind("SUPER + SHIFT + N", hl.dsp.exec_cmd("hyprnotes --show-organizer"), { description = "Hyprnotes: organizer" })

-- Optional autostart (only if you do NOT use the in-app XDG autostart setting).
-- hl.on("hyprland.start", function() hl.exec_cmd("hyprnotes --background") end)
