-- Snippets: one trigger per line of the "snippets" setting, written  trigger=expansion  (\n inside an expansion is a new line).
-- Triggers are registered when the plugin loads: after editing the setting, press Reload on the Plugins page.
-- No permissions needed.

hn.setting{
  id = "snippets", type = "string", title = "Snippets, one per line: ::trigger=text",
  default = "::sig=Best regards,\\nYour Name\n::shrug=\xC2\xAF\\_(\xE3\x83\x84)_/\xC2\xAF\n::todo=- [ ] ",
}

local count = 0
for line in (hn.settings.get("snippets") .. "\n"):gmatch("(.-)\n") do
  local trigger, text = line:match("^(::[%w_%-]+)=(.*)$")
  if trigger and #trigger >= 3 and #trigger <= 64 and count < 60 then
    local expansion = text:gsub("\\n", "\n")
    -- pcall: a duplicate trigger in the list must not stop the others from loading
    if pcall(hn.trigger, { pattern = trigger, replace = function() return expansion end }) then
      count = count + 1
    else
      hn.log("skipped duplicate or invalid trigger " .. trigger)
    end
  elseif line ~= "" then
    hn.log("ignored snippet line: " .. line:sub(1, 40))
  end
end
hn.log("snippets loaded: " .. count)
