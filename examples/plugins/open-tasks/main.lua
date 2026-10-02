-- Open Tasks: a panel fed by hn.notes.query{ from = "tasks" }.
-- Permissions: ui.panel (the panel), notes.index (the query), notes.read (open a note on click).
-- The query is a table, never SQL: the app validates it and runs it read-only with a 100 ms limit.

local LIMIT = 150   -- a panel holds at most 200 blocks

hn.panel{
  id = "tasks",
  title = "Open tasks",
  icon = "checklist",
  refresh_on = { "note.saved" },
  render = function()
    local rows, err = hn.notes.query{
      from = "tasks",
      where = { { field = "done", op = "=", value = false } },
      order = { { field = "path" }, { field = "line" } },
      select = { "path", "line", "text" },
      limit = LIMIT,
    }
    if not rows then return { { type = "empty", text = "The task index is not available (" .. tostring(err) .. ")." } } end
    if #rows == 0 then return { { type = "empty", text = "No open tasks. Nice." } } end
    local items = {}
    for i, row in ipairs(rows) do
      items[i] = {
        type = "item",
        title = row.text,
        subtitle = row.path,
        path = row.path,
        line = row.line,
        on_click = function() hn.notes.open(row.path) end,
      }
    end
    return {
      { type = "heading", text = #rows .. (#rows >= LIMIT and "+" or "") .. " open tasks" },
      { type = "button", label = "Refresh", on_click = function() hn.panel_refresh("tasks") end },
      { type = "list", items = items },
    }
  end,
}
