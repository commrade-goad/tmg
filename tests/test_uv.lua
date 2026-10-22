local uv = require("uv")

local function check(cond, msg)
    if not cond then error(msg or "check failed", 2) end
end

do
    local fired = 0
    local timer = uv.timer()
    local ok, err = timer:start(5, 0, function()
        fired = fired + 1
        timer:close()
    end)
    check(ok, err)
    uv.run()
    check(fired == 1, "timer did not fire")
    check(timer:is_closing(), "timer did not close")
end

do
    local fired = 0
    local timers = {}
    for i = 1, 3 do
        local timer = uv.timer()
        timers[i] = timer
        local ok, err = timer:start(i * 5, 0, function()
            fired = fired + 1
            timer:close()
        end)
        check(ok, err)
    end
    uv.run()
    check(fired == 3, "multiple timers did not all fire")
    for i = 1, 3 do check(timers[i]:is_closing(), "timer not closed") end
end

do
    local server = uv.tcp()
    local ok, err = server:bind("127.0.0.1", 0)
    check(ok, err)
    local host, port = server:getsockname()
    check(host == "127.0.0.1" and port > 0, "bad server address")

    local server_conn
    local got = ""
    ok, err = server:listen(16, function(client)
        check(client, "accept failed")
        server_conn = client
        client:read_start(function(read_err, data)
            if read_err then
                client:close()
                return
            end
            client:write("echo:" .. data)
        end)
    end)
    check(ok, err)

    local client = uv.tcp()
    ok, err = client:connect(host, port, function(connect_err)
        check(not connect_err, connect_err)
        client:write("hello", function(write_err)
            check(not write_err, write_err)
            client:read_start(function(read_err, data)
                if read_err then
                    check(read_err == "EOF", read_err)
                    check(got == "echo:hello", "bad echo: " .. got)
                    client:close()
                    return
                end
                got = got .. data
                if got == "echo:hello" then
                    client:close()
                    server:close()
                    if server_conn then server_conn:close() end
                end
            end)
        end)
    end)
    check(ok, err)
    uv.run()
    check(client:is_closing() and server:is_closing(), "tcp handles not closed")
end

do
    local server = uv.tcp()
    local ok, err = server:bind("127.0.0.1", 0)
    check(ok, err)
    local host, port = server:getsockname()
    ok, err = server:listen(16, function(client)
        server:close()
        client:read_start(function(read_err)
            if read_err then client:close() end
        end)
    end)
    check(ok, err)

    local write_done = false
    local close_done = false
    local client = uv.tcp()
    ok, err = client:connect(host, port, function(connect_err)
        check(not connect_err, connect_err)
        client:write("bye", function()
            write_done = true
        end)
        client:close(function()
            close_done = true
        end)
    end)
    check(ok, err)
    uv.run()
    check(write_done, "write callback missing after close")
    check(close_done, "close callback missing")
end

do
    local called = false
    local timer = uv.timer()
    local ok, err = timer:start(50, 0, function() called = true end)
    check(ok, err)
    timer:close(function() end)
    uv.run()
    check(not called, "closed timer callback fired")
    check(timer:is_closing(), "closed timer still active")
end

do
    local timer = uv.timer()
    local ok, err = timer:start(1000, 0, function() error("dropped timer fired") end)
    check(ok, err)
    timer = nil
    collectgarbage()
    collectgarbage()
    uv.run()
    check(not uv.loop_alive(), "dropped timer kept loop alive")
end

do
    local http = require("http")
    http.get("/health", function(req, res) res:send("ok") end)
    local ok, err = http.listen({
        host = "127.0.0.1",
        port = 18083,
        keepalive_timeout = 1,
    })
    check(ok, err)

    local body = ""
    local client = uv.tcp()
    ok, err = client:connect("127.0.0.1", 18083, function(connect_err)
        check(not connect_err, connect_err)
        client:write(
            "GET /health HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n",
            function(write_err)
                check(not write_err, write_err)
                client:read_start(function(read_err, data)
                    if read_err then
                        check(read_err == "EOF", read_err)
                        check(body:find(" 200 ", 1, true), "missing 200")
                        check(body:find("\r\n\r\nok", 1, true), "missing body")
                        client:close()
                        http.stop()
                        return
                    end
                    body = body .. data
                end)
            end)
    end)
    check(ok, err)
    uv.run()
    check(client:is_closing(), "http client not closed")
end

print("ok: uv tests")
