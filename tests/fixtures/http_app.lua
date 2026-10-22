local http = require("http")

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

http.get("/echo-cookie", function(req, res)
    res:send(req.cookies.session_id or "missing")
end)

http.post("/form", function(req, res)
    res:send((req.form.name or "?") .. ":" .. (req.form.age or "?"))
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
    keepalive_timeout = 1,
    upload_dir = "tests/uploads",
    template_root = "tests/fixtures/templates",
    debug = true,
})
if not ok then
    error("listen failed")
end
