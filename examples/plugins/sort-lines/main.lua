-- Sort Lines: acts on the selection. Permissions: note.read, note.edit (one undo step), ui.

hn.setting{ id = "ignore_case", type = "bool", default = true, title = "Ignore upper/lower case" }

local function sort_selection(descending)
  local sel = hn.note.selection()
  if sel == "" then
    hn.ui.notify("Select the lines to sort first")
    return
  end
  local trailing = sel:sub(-1) == "\n"
  if trailing then sel = sel:sub(1, -2) end

  local rows, fold = {}, hn.settings.get("ignore_case")
  for line in (sel .. "\n"):gmatch("(.-)\n") do
    rows[#rows + 1] = { text = line, key = fold and line:lower() or line, pos = #rows + 1 }
  end
  -- table.sort is not stable: the original position breaks ties.
  table.sort(rows, function(a, b)
    if a.key ~= b.key then
      if descending then return a.key > b.key end
      return a.key < b.key
    end
    return a.pos < b.pos
  end)

  local out = {}
  for i, r in ipairs(rows) do out[i] = r.text end
  hn.note.replace_selection(table.concat(out, "\n") .. (trailing and "\n" or ""))
  hn.ui.notify(string.format("Sorted %d lines", #rows))
end

hn.command{ id = "asc", title = "Sort selected lines (A to Z)", run = function() sort_selection(false) end }
hn.command{ id = "desc", title = "Sort selected lines (Z to A)", run = function() sort_selection(true) end }
