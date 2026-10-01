local function try(url, ...)
  local ok, a, b = pcall(hn.http.get, url, ...)
  if ok and type(a) == "table" then
    hn.log("res|" .. url .. "|ok|" .. tostring(a.status) .. "|" .. a.body)
  else
    hn.log("res|" .. url .. "|" .. (ok and "nil" or "error") .. "|" .. tostring(ok and b or a))
  end
end

hn.command{ id = "allowed", title = "Allowed", run = function()
  try("https://api.example.com/v1/items")
  try("https://cdn.example.org/file")
end }
hn.command{ id = "denied", title = "Denied", run = function()
  try("http://api.example.com/plain")
  try("https://evil.example.com/")
  try("https://api.example.com.evil.net/")
  try("https://user:pw@api.example.com/")
  try("https://api.example.com:8443/")
  try("https://127.0.0.1/")
  try("ftp://api.example.com/")
  try("https://api.example.com/\r\nX: y")
end }
hn.command{ id = "flood", title = "Flood", run = function()
  for i = 1, 25 do try("https://api.example.com/" .. i) end
end }
hn.command{ id = "headers", title = "Headers", run = function()
  try("https://api.example.com/h", { headers = { ["X-Token"] = "abc" } })
  try("https://api.example.com/h", { headers = { Host = "evil.com" } })
end }
