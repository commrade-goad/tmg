local http = require("http")

local sessions = {}
local session_seq = 0

local function new_session_id()
    session_seq = session_seq + 1
    return string.format("%x-%x", os.time(), session_seq)
end

local function session_for(req, res)
    local sid = req.cookies.todo_sid
    local session = sid and sessions[sid]
    if session then
        return session
    end

    sid = new_session_id()
    session = { todos = {}, next_id = 1 }
    sessions[sid] = session
    res:cookie("todo_sid", sid, {
        path = "/",
        httponly = true,
        samesite = "Lax",
    })
    return session
end

local function clean_title(value)
    local title = (value or ""):match("^%s*(.-)%s*$")
    if not title or title == "" or #title > 200 then
        return nil
    end
    return title
end

local function todo_id(value)
    local id = tonumber(value)
    if not id or id < 1 or id ~= math.floor(id) then
        return nil
    end
    return id
end

local function find_todo(session, id)
    for index, todo in ipairs(session.todos) do
        if todo.id == id then
            return todo, index
        end
    end
    return nil
end

local function render_list(res, session)
    res:render("list.tmg", session.todos)
end

local function render_edit(res, todo)
    res:render("edit.tmg", todo)
end

http.get("/health", function(req, res)
    res:send("ok")
end)

http.get("/", function(req, res)
    session_for(req, res)
    res:render("index.tmg")
end)

http.get("/todos", function(req, res)
    local session = session_for(req, res)
    render_list(res, session)
end)

http.post("/todos", function(req, res)
    local session = session_for(req, res)
    local title = clean_title(req.form.title)
    if not title then
        res:status(400):send("Title is required")
        return
    end

    local todo = { id = session.next_id, title = title, done = false }
    session.next_id = session.next_id + 1
    table.insert(session.todos, todo)
    render_list(res, session)
end)

http.get("/todos/:id/edit", function(req, res)
    local session = session_for(req, res)
    local id = todo_id(req.params.id)
    local todo = id and find_todo(session, id)
    if not todo then
        res:status(404):send("Todo not found")
        return
    end
    render_edit(res, todo)
end)

http.put("/todos/:id", function(req, res)
    local session = session_for(req, res)
    local id = todo_id(req.params.id)
    local todo = id and find_todo(session, id)
    if not todo then
        res:status(404):send("Todo not found")
        return
    end

    local title = clean_title(req.form.title)
    if not title then
        res:status(400):send("Title is required")
        return
    end

    todo.title = title
    render_list(res, session)
end)

http.post("/todos/:id/toggle", function(req, res)
    local session = session_for(req, res)
    local id = todo_id(req.params.id)
    local todo = id and find_todo(session, id)
    if not todo then
        res:status(404):send("Todo not found")
        return
    end

    todo.done = not todo.done
    render_list(res, session)
end)

http.delete("/todos/:id", function(req, res)
    local session = session_for(req, res)
    local id = todo_id(req.params.id)
    local todo, index
    if id then
        todo, index = find_todo(session, id)
    end
    if not todo then
        res:status(404):send("Todo not found")
        return
    end

    table.remove(session.todos, index)
    render_list(res, session)
end)

http.static("/static", "tests/htmx_todo/public")

http.listen {
    host = "127.0.0.1",
    port = 18081,
    max_body = 256 * 1024,
    max_static_size = 2 * 1024 * 1024,
    keepalive_timeout = 5,
    template_root = "tests/htmx_todo/templates",
}
