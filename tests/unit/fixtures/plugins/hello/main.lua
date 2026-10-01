local util = require("lib.util")

hn.setting{ id = "greeting", type = "string", default = "Hello", title = "Greeting" }
hn.setting{ id = "loud", type = "bool", default = false, title = "Loud" }

hn.command{
  id = "shout", title = "Shout selection",
  run = function()
    local sel = hn.note.selection()
    hn.note.replace_selection(util.shout(sel))
    hn.note.insert("!")
  end,
}
hn.command{ id = "count", title = "Count runs", run = function()
  local n = (hn.storage.get("n") or 0) + 1
  hn.storage.set("n", n)
  hn.log("count=" .. n)
end }
hn.command{ id = "fail", title = "Always fails", run = function() error("boom") end }
hn.command{ id = "version", title = "Version", run = function() hn.log("version=" .. hn.version) end }
hn.toolbar_button{ id = "tb", title = "Toolbar", icon = "star", run = function() hn.log("toolbar") end }
hn.menu_item{ id = "mi", title = "Menu", where = "note", run = function() hn.log("menu") end }
hn.trigger{ pattern = "::date", replace = function() return "2026-10-01" end }
hn.trigger{ pattern = "::hello", replace = function() return hn.settings.get("greeting") end }

hn.on("note.opened", function(path) hn.log("opened " .. path) end)
hn.on("note.changed", function(path) hn.log("changed " .. path) end)
hn.on("note.saved", function(path) hn.log("saved " .. path) end)
hn.on("note.pre_save", function(text) return (text:gsub("%s+$", "")) .. "\n" end)
