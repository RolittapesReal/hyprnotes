-- Word Count: one command plus an optional "after every save" notification.
-- Permissions: note.read (read the note), ui (show the notification).

hn.setting{ id = "notify_on_save", type = "bool", default = false, title = "Show the word count after every save" }

local function count(text)
  local words = 0
  for _ in text:gmatch("%S+") do words = words + 1 end
  local lines = text == "" and 0 or select(2, text:gsub("\n", "")) + (text:sub(-1) == "\n" and 0 or 1)
  return words, utf8.len(text) or #text, lines
end

local function report(text, what)
  local words, chars, lines = count(text)
  hn.ui.notify(string.format("%s: %d words, %d characters, %d lines", what, words, chars, lines))
end

hn.command{
  id = "count",
  title = "Word count",
  run = function()
    local sel = hn.note.selection()
    if sel ~= "" then report(sel, "Selection") else report(hn.note.text(), "Note") end
  end,
}

hn.on("note.saved", function()
  if hn.settings.get("notify_on_save") then report(hn.note.text(), "Saved") end
end)
