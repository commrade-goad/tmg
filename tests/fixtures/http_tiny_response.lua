http.get("/ok", function(req, res) res:send("ok") end)

local ok = http.listen({
    host = "127.0.0.1",
    port = 18086,
    max_response = 64,
    keepalive_timeout = 1,
    debug = true,
})
if not ok then
    error("listen failed")
end
