local http = require("http")

http.get("/health", function(req, res)
    res:send("ok")
end)

http.listen({
    port = 18082,
    max_body = -1,
})
