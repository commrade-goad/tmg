local http = require("http")

local cache = os.getenv("TMG_TEST_CACHE") or "off"
if cache ~= "development" and cache ~= "production" then
    cache = false
end

tmg_delim = "#"

http.get("/health", function(req, res)
    res:send("ok")
end)

http.get("/hash", function(req, res)
    tmg_delim = "#"
    res:render("hash.tmg", "world")
end)

http.get("/at", function(req, res)
    tmg_delim = "@@"
    res:render("at.tmg", "world")
end)

http.listen {
    host = "127.0.0.1",
    port = 18088,
    keepalive_timeout = 5,
    template_root = "tests/fixtures/templates-sep",
    template_cache = cache,
}
