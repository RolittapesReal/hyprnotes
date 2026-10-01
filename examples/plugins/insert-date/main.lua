-- Insert Date: typing triggers. A trigger's `replace` function returns the text that replaces what was typed.
-- No permissions are needed: hn.time and hn.settings are always available.

hn.setting{ id = "date_format", type = "string", default = "%Y-%m-%d", title = "Date format (strftime)" }
hn.setting{ id = "time_format", type = "string", default = "%H:%M", title = "Time format (strftime)" }

-- A bad user-supplied format must never leave a trigger doing nothing: fall back to the default.
local function stamp(setting, default)
  local ok, text = pcall(hn.time.format, hn.settings.get(setting))
  if ok then return text end
  return hn.time.format(default)
end

hn.trigger{ pattern = "::date", replace = function() return stamp("date_format", "%Y-%m-%d") end }
hn.trigger{ pattern = "::time", replace = function() return stamp("time_format", "%H:%M") end }
