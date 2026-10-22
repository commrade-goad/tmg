# TMG
a Lua runtime with templating built in. Write a `build.lua` script that
calls into `tmg.render(...)` to expand `.tmg` templates — the templates
themselves still use `lua` to script the parts that expand.

## Integration
Generate base16 colors using wallpaper with [tmg-wall](https://github.com/commrade-goad/tmg-wall).

## Build
All third-party dependencies are cloned into `deps/` and built from source;
nothing is taken from the system package manager:

* libuv `v1.53.0`
* llhttp `release/v9.4.3` (pre-generated C, no Node needed)
* wslay `release-1.1.1` (WebSocket framing)
* LuaJIT in `LuaJIT-2.1/`

```sh
make deps          # optional: clone dependencies now
make all -j$(nproc)
```

`make` links `libluajit.a`, `libuv.a`, `libllhttp.a` and `libwslay.a`
statically into the `tmg` binary. libc remains dynamic; a fully static glibc binary is not
attempted. Verify with:

```sh
ldd ./tmg           # no libuv/llhttp/wslay/luajit shared libraries
```

## Usage
`tmg` does not render anything on its own — point it at a build script:

```sh
tmg build.lua
```

`build.lua` is plain Lua with a global `tmg` table available (also
`require("tmg")` works):

```lua
-- render "in.tmg" using "%" (the default delimiter) as the escape char,
-- writing the result to "out.conf". Returns true/false.
local ok = tmg.render("in.tmg", "out.conf")

-- override the delimiter for one call:
tmg.render("in.tmg", "out.conf", "#")

-- or set it for every render() call in this script:
tmg_delim = "#"

-- print each generated Lua chunk to stderr before running it, useful when
-- debugging a template:
tmg_debug = true
```

Inside a `.tmg` file, the delimiter char toggles between plain text and
embedded Lua (default `%`). Use `tmg.print(...)` inside a code block to emit
a value:

```
% local data = ... %
Hello % tmg.print(data.name) %!
```

`tmg.render_string(path, sep, data)` returns the rendered string, or
`nil, error`. The optional `data` value is passed to the template chunk as
its vararg, so `local data = ...` works without touching globals. A fourth
argument, `{ autoescape = true }`, HTML-escapes values emitted by
`tmg.print`; `tmg.raw(value)` bypasses escaping. CLI `tmg.render(...)` does
not auto-escape, so LaTeX and other plain-text templates keep working.

Because rendering happens in the same Lua state your build script is
running in, anything the script `require`s or sets as a global is visible
to every template it renders — set shared state once at the top of
`build.lua` instead of repeating it per template.

### Script arguments
Everything after the script name is passed to the script, as in plain Lua:

```sh
tmg build.lua prod --verbose "two words"
```

```lua
-- build.lua
local env = ...                -- varargs of the main chunk: "prod", "--verbose", "two words"
print(arg[0])                  -- "build.lua"
print(#arg, arg[1], arg[2])    -- 3  prod  --verbose
print(arg[-1])                 -- the tmg binary, as invoked
```

`arg` is a normal global, so templates rendered by the script can read it too.
`tmg`'s own options (`-h`, `-v`) are only recognized *before* the script name,
so `tmg build.lua -v` hands `-v` to your script.

## HTTP server
A libuv + llhttp HTTP/1.1 server is built in. Calling `http.listen` starts
it; after the script returns, `tmg` runs the event loop:

```lua
local http = require("http")

http.get("/", function(req, res)
    res:send("hello")
end)

-- serve GET/HEAD under /assets from ./public; routes still take precedence
http.static("/assets", "public")

-- :id becomes req.params.id
http.get("/tu/requests/:id", function(req, res)
    res:render("request.tmg", { id = req.params.id })
end)

http.post("/tu/requests/:id/approve", function(req, res)
    if req.cookies.session_id ~= "admin" then
        res:status(401):send("unauthorized")
        return
    end
    res:json({ approved = true, id = req.params.id })
end)

http.listen {
    host = "127.0.0.1",
    port = 8080,
    upload_dir = "uploads",       -- multipart file destination
    template_root = "templates",  -- res:render() is confined to this
    max_body = 10 * 1024 * 1024,
    max_static_size = 64 * 1024 * 1024,
    max_ws_message = 10 * 1024 * 1024,
    header_timeout = 10,
    keepalive_timeout = 15,
}
```

Routes can also be registered with `http.route(method, pattern, handler)`;
`http.put/patch/delete/head/options` mirror `get/post`.

WebSocket routes use the same pattern syntax and receive the normal `req`
table, including `req.params`:

```lua
http.ws("/ws/:room", function(ws, req)
    local room = req.params.room
    ws:on("message", function(ws, data, binary)
        ws:send(room .. ":" .. data, binary)
    end)
    ws:on("close", function(ws, code, reason)
        print("closed", code, reason)
    end)
end)
```

The handler is the open callback; it may call `ws:send` immediately or store
`ws` for later use. `ws:send(data, binary)` sends a text frame by default,
`ws:ping(data)` sends a ping, `ws:close(code, reason)` starts the close
handshake, and `ws:is_open()` reports whether the connection is still open.
Fragmented messages are reassembled, and ping/pong control frames are handled
by wslay. Incoming messages are limited by `max_ws_message`. Subprotocol and
extension negotiation is not implemented. A missing or invalid handshake gets
`400`; an unsupported `Sec-WebSocket-Version` gets `426` with
`Sec-WebSocket-Version: 13`.

`req` contains `method`, `url`, `path`, `query`, `version`, `headers`,
`cookies`, `body`, `params`, `query_params`, `form`, `files`, and
`remote_address`. Helpers: `req:header(name)`, `req:query(name)`,
`req:is_keep_alive()`, `req:remote_address()`. Uploaded multipart files are
saved under `upload_dir` and exposed as
`{ field, filename, content_type, path, size }` entries in `req.files`.

`res` methods: `status(code)`, `header(name, value)`, `send(body)`,
`json(value)`, `html(body)`, `redirect(location, status)`,
`cookie(name, value, opts)`, `render(template, data, opts)`, `close()`.
`res:cookie` defaults to `HttpOnly; SameSite=Lax; Path=/`; options include
`path`, `domain`, `max_age`, `expires`, `secure`, `httponly`, `samesite`.

`http.static(prefix, root)` maps a URL prefix to a document root. Only
`GET` and `HEAD` are served, paths are resolved under `root` with `..` and
symlink-escape checks, directory paths serve `index.html`, and files larger
than `max_static_size` are rejected. Range requests are not implemented.

`res:render` HTML-escapes dynamic values emitted by `tmg.print` by default,
including values from `data`; literal template markup is untouched. Use
`tmg.raw(value)` for trusted HTML or pass `{ autoescape = false }` to disable
it. `tmg.escape_html(value)` returns an escaped string for direct use outside
autoescaped output. Rendered templates default to
`text/html; charset=utf-8` when auto-escaping.

Every handler is invoked through a protected Lua call. A syntax/runtime
error returns HTTP 500 to that request, logs a traceback, and leaves the
server loop running. Set `debug = true` in `http.listen` to include the Lua
error text in the 500 body.

`http.stop()` closes the listener and signal handles; active responses finish
and idle keep-alive connections are closed. Sequential keep-alive is enabled
when the client asks for it, and idle sockets are reaped after
`keepalive_timeout` seconds. `tests/fixtures/http_app.lua` plus
`tests/test_http.py` are a runnable example (`make test`).

A session-scoped htmx Todo SPA lives in `tests/htmx_todo/`:

```sh
./tmg tests/htmx_todo/app.lua
# open http://127.0.0.1:18081
```

It serves htmx locally, uses server-rendered fragments, and keeps CRUD state
in memory per `todo_sid` cookie. `tests/htmx_todo/test.py` covers the full
create/read/update/delete flow.

## Extra library paths
Set `TMG_LIB_PATH` to a `:`-separated list of directories to make your own
Lua/LuaJIT modules available to `require` (from `build.lua` and from
templates alike):

```sh
TMG_LIB_PATH=$HOME/.local/share/tmg/lib:/opt/tmg-libs tmg build.lua
```

Each directory is searched for `name.lua`, `name/init.lua` and (C modules)
`name.so`. Entries are searched before the default paths, in the order
given. Two helpers are available on the `tmg` table for modules that wrap native
libraries (e.g. a LuaJIT ffi binding to raylib):

```lua
local ffi = require("ffi")
ffi.cdef[[ /* ... raylib declarations ... */ ]]

-- searches every TMG_LIB_PATH dir for libraylib.so, returns the full path
-- (or nil, err); a subpath is allowed: tmg.find_lib("native/raylib")
local rl = ffi.load(assert(tmg.find_lib("raylib")))

print(table.concat(tmg.lib_path, ":")) -- the parsed TMG_LIB_PATH dirs
```

`ffi.load("raylib")` on its own only uses the system loader
(`LD_LIBRARY_PATH`), which is why `find_lib` exists. A bare name is tried as
`lib<name>.so` then `<name>.so`; a name that already contains `.so`
(`libfoo.so.2`) is used literally.

## LaTeX library
`lib/latex.lua` builds LaTeX from Lua tables. Put `lib/` on `TMG_LIB_PATH`
and `require("latex")`:

```lua
local L = require("latex")
local doc = L.document{
  packages = { "booktabs", { "geometry", "margin=2cm" } },
  title = "Hello", author = "me",
  L.section("Intro", "50% & $5 are escaped. ", L.m"E = mc^2",
    L.itemize{ "one", "two", L.enumerate{ "nested under two" } }),
  L.table{ caption = "Results", cols = "lr", header = { "Name", "V" },
           rows = { { "a", 1 }, { "b", 2 } } },
}
L.write(doc, "out.tex")      -- or: print(L.render(doc))
L.compile("out.tex")         -- optional: latexmk -pdf, returns ok, errors
```

* Lua strings are text and get escaped; `L.raw"..."` is raw LaTeX. Options
  and math strings are raw. Plain tables are lists and are flattened.
* Anything not wrapped is available through two primitives:
  * `L.cmd("name", opts, arg1, arg2)` -> `\name[opts]{arg1}{arg2}`; every
    argument is its own `{group}` and is escaped text (use `L.raw`).
  * `L.env("name", spec, ...children)` -> `\begin{name}[opts]{args} ... \end{name}`,
    where `spec` is a raw string (`"htbp"`), or a table: its array part and
    `k=v` fields are the options, `args = { ... }` the mandatory arguments
    (plain strings are raw here: `"0.5\\textwidth"`, `"lr"`). The same as one table:
    `L.env{ "minipage", opts = "t", args = { "0.5\\textwidth" }, "text" }`.
  * Sugar for both: `L.c.textbf"x"`, `L.e.center{ ... }`,
    `L.e.minipage{ opts = "t", args = { ... }, "text" }`.
  * `L.item("text")` / `L.item{ term = "T", "text" }` is `\item`; inside an env
    (or `L.itemize{}`) every item gets its own indented line.
* Every node is a string when you need one: `tostring(node)`, `..`, `L.render(node)`.

### In templates
`L.print(node)` renders a node into the template through `tmg.print`
(`tmg.print(node)` works as well, it honors `__tostring`). `%` starts a
comment in LaTeX, so pick another delimiter, e.g. `tmg_delim = "@@"`:

```
\section{Results}
@@ L.print(L.table{ header = { "a", "b" }, rows = data }) @@
```

Hand-written LaTeX stays untouched, generated parts come from Lua. A code
block ends at the next delimiter, so don't write it inside Lua comments.
See `example/latex-build.lua` and `example/latex-demo.tmg`.

## raylib
`lib/raylib-luajit-generated` is a submodule with LuaJIT ffi bindings for raylib
(`git submodule update --init`). `lib/raylib.lua` is the glue that makes
`require("raylib")` work from any directory: the binding itself expects to run
from its own repo root and only uses the system library path.

```sh
TMG_LIB_PATH=/path/to/tmg/lib:/dir/with/libs tmg example/raylib-demo.lua
```

* `libraylib.so` (raylib 5.5, shared build) is looked up in `TMG_LIB_PATH`, then
  in the binding's `raylib/` dir, then the system paths (`LD_LIBRARY_PATH`...).
* `libphysac.so` is optional; without it a warning is printed and the physac
  functions are unavailable. See the binding's README for building it.
* Set `RAYLIB_LUAJIT_DIR` if the binding lives somewhere else.
