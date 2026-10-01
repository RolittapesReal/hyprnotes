-- Title Case: capitalise each word of the selection except small words (not at the start or end).
-- Works on ASCII letters; other characters are left alone. Permissions: note.read, note.edit, ui.

local small = {}
for w in ("a an and as at but by for in nor of on or per the to via vs"):gmatch("%a+") do small[w] = true end

local function cap(word)
  return (word:gsub("^(%p*)(%a)", function(pre, c) return pre .. c:upper() end))
end

local function title_case(text)
  local words = {}
  text:gsub("%S+", function(w) words[#words + 1] = w end)
  local i, after_colon = 0, false
  return (text:gsub("%S+", function(w)
    i = i + 1
    local lower = w:lower()
    local keep = i > 1 and i < #words and not after_colon and small[lower:gsub("%p", "")]
    after_colon = w:sub(-1) == ":"
    if keep then return lower end
    -- capitalise each part of a hyphenated word too
    return (lower:gsub("[^%-]+", cap))
  end))
end

hn.command{
  id = "title-case",
  title = "Title Case selection",
  run = function()
    local sel = hn.note.selection()
    if sel == "" then
      hn.ui.notify("Select some text first")
      return
    end
    hn.note.replace_selection(title_case(sel))
  end,
}
