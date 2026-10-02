-- Backlinks Panel: a declarative panel (the app draws it) fed by the note index.
-- Permissions: ui.panel (the panel), note.read (ctx.path = the open note), notes.index (backlinks), notes.read (open a note on click).

hn.setting{ id = "max_rows", type = "number", default = 50, title = "Most backlinks to show (1-150)" }

local function name_of(path)
  return (path:match("([^/]+)$") or path):gsub("%.md$", "")
end

hn.panel{
  id = "backlinks",
  title = "Backlinks",
  icon = "link",
  refresh_on = { "note.opened", "note.saved" },
  render = function(ctx)
    if not ctx.path then return { { type = "empty", text = "Open a note to see what links to it." } } end
    local limit = math.max(1, math.min(150, math.floor(hn.settings.get("max_rows") or 50)))
    local rows, err = hn.notes.backlinks(ctx.path, { limit = limit })
    if not rows then return { { type = "empty", text = "The link index is not available (" .. tostring(err) .. ")." } } end
    if #rows == 0 then return { { type = "empty", text = "Nothing links to " .. (ctx.title or ctx.path) .. " yet." } } end
    local items = {}
    for i, row in ipairs(rows) do
      local src = row.src
      items[i] = {
        type = "item",
        title = name_of(src),
        subtitle = row.context,
        path = src,
        line = row.line > 0 and row.line or nil,
        on_click = function() hn.notes.open(src) end,
      }
    end
    return {
      { type = "heading", text = #rows .. (#rows == 1 and " backlink" or " backlinks"), level = 2 },
      { type = "list", items = items },
    }
  end,
}
