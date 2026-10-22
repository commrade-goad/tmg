local http = require("http")
local saved_res

http.get("/", function(req, res)
    res:send("hello")
end)

http.get("/health", function(req, res)
    res:send("ok")
end)

http.get("/tu/requests/:id", function(req, res)
    res:send("id=" .. req.params.id)
end)

http.post("/tu/requests/:id/approve", function(req, res)
    res:send("approved " .. req.params.id)
end)

http.get("/tu/requests/:id/pdf", function(req, res)
    res:send("pdf " .. req.params.id)
end)

http.get("/boom", function()
    error("intentional handler failure")
end)

http.get("/set-cookie", function(req, res)
    res:cookie("session_id", "abc123", {
        path = "/",
        httponly = true,
        samesite = "Lax",
        max_age = 3600,
    })
    res:send("cookie set")
end)

http.get("/cookie-invalid", function(req, res)
    local ok, err = pcall(function()
        res:cookie("session_id", "abc123", { secure = "yes" })
    end)
    res:send(ok and "unexpected" or tostring(err))
end)

http.get("/echo-cookie", function(req, res)
    res:send(req.cookies.session_id or "missing")
end)

http.post("/form", function(req, res)
    res:send((req.form.name or "?") .. ":" .. (req.form.age or "?"))
end)

http.get("/render-cache", function(req, res)
    res:render("cache_test.tmg", { name = "cache" })
end)

http.get("/cache-clear", function(req, res)
    http.template_cache_clear()
    res:send("cleared")
end)

http.get("/cache-stats", function(req, res)
    local s = http.template_cache_stats()
    res:send(string.format("%s,%d,%d,%d,%d,%d", s.mode, s.entries,
                            s.hits, s.misses, s.reloads, s.evictions))
end)

http.post("/upload", function(req, res)
    local f = req.files[1]
    if not f then
        res:status(400):send("no file")
        return
    end
    res:send(f.field .. "|" .. f.filename .. "|" .. f.content_type .. "|" .. f.size .. "|" .. f.path)
end)

http.get("/render", function(req, res)
    res:render("hello.tmg", { name = "TMG" })
end)

http.get("/escape", function(req, res)
    res:render("escape.tmg", { text = [[<b>"&'</b>]], raw = "<i>raw</i>" })
end)

http.get("/escape-off", function(req, res)
    res:render("escape.tmg", { text = [[<b>"&'</b>]], raw = "<i>raw</i>" }, { autoescape = false })
end)

http.get("/stash-response", function(req, res)
    saved_res = res
    res:send("stored")
end)

http.get("/use-stale-response", function(req, res)
    local ok, err = pcall(function()
        saved_res:send("late")
    end)
    if ok then
        res:send("unexpected")
    else
        res:send(tostring(err))
    end
end)

http.get("/too-large-response", function(req, res)
    res:send(string.rep("x", 300 * 1024))
end)

http.get("/render-too-large", function(req, res)
    res:render("hello.tmg", { name = string.rep("x", 300 * 1024) })
end)

http.ws("/ws/:room", function(ws, req)
    local room = req.params.room
    ws:on("message", function(ws, data, binary)
        ws:send(room .. ":" .. (binary and "bin:" or "txt:") .. data, binary)
    end)
    ws:on("close", function(ws, code, reason)
        -- no application cleanup needed in this example
    end)
end)

http.ws("/ws/hello", function(ws)
    ws:send("welcome")
end)

http.ws("/ws/close-on-message", function(ws)
    ws:on("message", function(ws)
        ws:close(1000, "done")
    end)
end)

http.get("/shutdown", function(req, res)
    res:send("bye")
    http.stop()
end)

http.static("/static", "tests/fixtures/public")

local ok = http.listen({
    host = "127.0.0.1",
    port = 18080,
    max_body = 2 * 1024 * 1024,
    max_static_size = 1024 * 1024,
    max_response = 256 * 1024,
    keepalive_timeout = 1,
    upload_dir = "tests/uploads",
    template_root = "tests/fixtures/templates",
    debug = true,
})
if not ok then
    error("listen failed")
end
