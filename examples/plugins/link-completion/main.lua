-- Link Completion: hn.complete on "[[" offering note names that actually resolve.
-- Permissions: editor.complete (the popup), notes.read (the list of notes), notes.index (check that a name is unambiguous).
-- Budget: 20 ms of Lua per request (time spent inside the index calls is accounted separately), at most 50 items.

local MAX = 20

local function stem(path) return (path:match("([^/]+)$") or path):gsub("%.md$", "") end

hn.complete{
  id = "wiki",
  trigger = "[[",
  items = function(query)
    local items = {}
    for _, note in ipairs(hn.notes.list(query)) do
      if #items >= MAX then break end
      local name = stem(note.path)
      local r = hn.notes.resolve(name)               -- nil on timeout / unsupported: then just use the plain name
      local insert = name
      if r and r.status == "ambiguous" then insert = note.path:gsub("%.md$", "") end   -- folder/name stays unique
      items[#items + 1] = {
        label = note.title ~= "" and note.title or name,
        detail = note.path,
        insert = insert .. "]]",
      }
    end
    return items
  end,
}
