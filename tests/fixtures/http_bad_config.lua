local http = require("http")

http.get("/health", function(req, res)
    res:send("ok")
end)

local case = os.getenv("TMG_BAD_CONFIG") or "negative"
local opts = { port = 18082 }

if case == "negative" then
    opts.max_body = -1
elseif case == "timeout_overflow" then
    opts.header_timeout = 2 ^ 40
elseif case == "size_type" then
    opts.max_body = "not a number"
elseif case == "fraction" then
    opts.max_body = 1.5
elseif case == "bad_port" then
    opts.port = 70000
end

http.listen(opts)
