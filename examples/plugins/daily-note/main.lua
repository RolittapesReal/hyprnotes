-- Daily Note: makes sure today's dated note exists. Permissions: notes.read (is it there already?),
-- notes.write (create it; this is the dangerous one - it can also overwrite and delete notes), ui.

hn.setting{ id = "folder", type = "string", default = "daily", title = "Folder for daily notes" }
hn.setting{ id = "template", type = "string", default = "# {{date}}\n\n## Plan\n\n## Notes\n", title = "Template ({{date}} is replaced; \\n is a new line)" }

local function today_path()
  local folder = hn.settings.get("folder"):gsub("^/+", ""):gsub("/+$", "")
  local name = hn.time.format("%Y-%m-%d") .. ".md"
  return (folder ~= "" and (folder .. "/") or "") .. name
end

hn.command{
  id = "today",
  title = "Create or find today's note",
  run = function()
    local path = today_path()
    if hn.notes.read(path) then
      hn.ui.notify("Today's note already exists: " .. path)
      return
    end
    local text = hn.settings.get("template"):gsub("\\n", "\n"):gsub("{{date}}", hn.time.format("%Y-%m-%d"))
    if hn.notes.write(path, text) then
      hn.ui.notify("Created " .. path)
    else
      hn.ui.notify("Could not create " .. path)
    end
  end,
}
