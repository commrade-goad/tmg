local http = require("http")

local sessions = {}
local session_seq = 0

local function new_session_id()
    session_seq = session_seq + 1
    return string.format("counter-%x-%x", os.time(), session_seq)
end

local function session_for(req, res)
    local sid = req.cookies.counter_sid
    local session = sid and sessions[sid]
    if session then
        return session
    end

    sid = new_session_id()
    session = { count = 0 }
    sessions[sid] = session
    res:cookie("counter_sid", sid, {
        path = "/",
        httponly = true,
        samesite = "Lax",
    })
    return session
end

http.get("/health", function(req, res)
    res:send("ok")
end)

http.get("/", function(req, res)
    local session = session_for(req, res)
    res:render("index.tmg", session.count)
end)

http.get("/counter", function(req, res)
    local session = session_for(req, res)
    res:render("counter.tmg", session.count)
end)

http.post("/counter/increment", function(req, res)
    local session = session_for(req, res)
    session.count = session.count + 1
    res:render("counter.tmg", session.count)
end)

http.static("/static", "tests/react_counter/public")
http.static("/vendor", "tests/htmx_todo/public")

http.listen {
    host = "127.0.0.1",
    port = 18087,
    max_body = 16 * 1024,
    max_static_size = 2 * 1024 * 1024,
    keepalive_timeout = 5,
    template_root = "tests/react_counter/templates",
}
