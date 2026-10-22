local http = require("http")

http.get("/health", function(req, res) res:send("ok") end)

http.post("/echo", function(req, res) res:send(req.body or "") end)

http.get("/echo-path/:value", function(req, res)
    res:send(req.params.value or "missing")
end)

http.get("/big", function(req, res)
    res:send(string.rep("x", 512))
end)

http.get("/shutdown", function(req, res)
    res:send("bye")
    http.stop()
end)

http.post("/upload", function(req, res)
    local f = req.files[1]
    if f then
        res:send(f.filename .. ":" .. f.size)
    else
        res:send("nofile")
    end
end)

http.post("/form", function(req, res)
    res:send((req.form.name or "?") .. ":" .. (req.form.last or "?"))
end)

http.ws("/ws", function(ws)
    ws:on("message", function(ws, data)
        ws:send(data)
    end)
end)

http.static("/static", "tests/fixtures/public")

local ok = http.listen({
    host = "127.0.0.1",
    port = 18085,
    max_body = 2048,
    max_header_bytes = 256,
    max_headers = 8,
    max_url = 64,
    max_static_size = 32,
    max_ws_message = 32,
    max_response = 256,
    max_multipart_parts = 4,
    max_multipart_files = 2,
    keepalive_timeout = 1,
    upload_dir = "tests/uploads-limits",
    template_root = "tests/fixtures/templates",
    debug = true,
})
if not ok then
    error("listen failed")
end
