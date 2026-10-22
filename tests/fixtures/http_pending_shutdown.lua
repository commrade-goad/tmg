local http = require("http")

http.get("/health", function(req, res)
    res:send("ok")
end)

http.get("/big", function(req, res)
    res:send(string.rep("q", 4 * 1024 * 1024))
end)

http.get("/shutdown-big", function(req, res)
    res:send(string.rep("z", 4 * 1024 * 1024))
    http.stop()
end)

local ok = http.listen({
    host = "127.0.0.1",
    port = 18084,
    max_response = 8 * 1024 * 1024,
    keepalive_timeout = 1,
})
if not ok then
    error("listen failed")
end
