-- Markdown TOC: reads the headings of the note and inserts a nested, linked list where the cursor is.
-- Permissions: note.read, note.edit, ui.

hn.setting{ id = "max_depth", type = "number", default = 3, title = "Deepest heading level to include (1-6)" }
hn.setting{ id = "bullet", type = "string", default = "-", title = "List marker" }

-- GitHub-style anchor: lower case, drop punctuation, spaces become dashes, repeats get -1, -2 ...
local function slugger()
  local seen = {}
  return function(title)
    local s = title:lower():gsub("[^%w%s%-_]", ""):gsub("%s", "-")
    local n = seen[s]
    seen[s] = (n or 0) + 1
    return n and (s .. "-" .. n) or s
  end
end

local function headings(text, max_depth)
  local found, fence = {}, nil
  local slug = slugger()
  for line in (text .. "\n"):gmatch("(.-)\n") do
    local f = line:match("^%s*(```+)") or line:match("^%s*(~~~+)")
    if f then
      if not fence then fence = f:sub(1, 1) elseif f:sub(1, 1) == fence then fence = nil end
    elseif not fence then
      local hashes, title = line:match("^(#+)%s+(.-)%s*#*%s*$")
      if hashes and #hashes <= 6 and title ~= "" then
        local anchor = slug(title)
        if #hashes <= max_depth then found[#found + 1] = { level = #hashes, title = title, anchor = anchor } end
      end
    end
  end
  return found
end

hn.command{
  id = "insert",
  title = "Insert table of contents",
  run = function()
    local depth = math.max(1, math.min(6, math.floor(hn.settings.get("max_depth"))))
    local list = headings(hn.note.text(), depth)
    if #list == 0 then
      hn.ui.notify("No headings found")
      return
    end
    local base, out, bullet = list[1].level, {}, hn.settings.get("bullet")
    for _, h in ipairs(list) do base = math.min(base, h.level) end
    for _, h in ipairs(list) do
      out[#out + 1] = string.rep("  ", h.level - base) .. bullet .. " [" .. h.title .. "](#" .. h.anchor .. ")"
    end
    hn.note.insert(table.concat(out, "\n") .. "\n")
    hn.ui.notify("Inserted a table of contents with " .. #list .. " entries")
  end,
}
