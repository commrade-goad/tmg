Understood. I was applying a multi-tenant/untrusted-code threat model that does not fit your project. For TMG, assume:

- the Lua application and templates are trusted;
- the server runs locally/on the user’s machine;
- shared Lua globals are allowed by convention;
- no sandbox is required;
- the goal is a practical LuaJIT + template + HTTP runtime.

Here is the consolidated implementation master list.

# TMG HTTP Master Implementation List

## 1. libuv integration

Add a C-backed Lua module for:

- `uv_loop_t`
- TCP server creation
- TCP client connections
- accept callbacks
- read callbacks
- write callbacks
- connection close callbacks
- timers
- optional filesystem operations
- optional DNS operations
- loop execution and stopping

Suggested Lua shape:

```lua
local uv = require("uv")

local server = uv.tcp()
server:bind("127.0.0.1", 8080)
server:listen(function(client)
    -- connection handling
end)

uv.run()
```

Requirements:

- use the default loop initially;
- keep the loop alive while active handles exist;
- close handles correctly;
- do not call Lua APIs from worker threads;
- make callbacks execute on the Lua/event-loop thread;
- support graceful loop shutdown;
- expose useful libuv error messages.

## 2. llhttp integration

Add llhttp as the HTTP/1 parser.

Handle:

- request method;
- URL/path;
- query string;
- HTTP version;
- header names;
- header values;
- body chunks;
- message completion;
- parser errors;
- keep-alive detection;
- `Content-Length`;
- chunked transfer encoding;
- connection-close semantics.

Do not assume one TCP read equals one HTTP request. The parser must support:

- partial request lines;
- split headers;
- split header values;
- split bodies;
- multiple requests on one keep-alive connection.

Configure limits for:

- request line length;
- URL length;
- total header bytes;
- individual header length;
- header count;
- request body size.

## 3. Connection state

Create a per-connection C structure containing at least:

```text
uv_tcp_t
llhttp_t
llhttp_settings_t
read buffer
parser state
current request
current response
Lua callback references
Lua request coroutine/reference
pending writes
connection flags
timeouts
```

Track states such as:

```text
CONNECTED
READING
PARSING
HANDLING
WRITING
KEEP_ALIVE
CLOSING
CLOSED
```

Requirements:

- one parser per TCP connection;
- no parser state shared between clients;
- safely handle multiple requests on one connection;
- close the connection on fatal parser errors;
- clean up everything when the client disconnects.

## 4. Lua callback lifetime

Use Lua registry references for every Lua value retained by C or libuv:

- route handlers;
- server callbacks;
- connection callbacks;
- request callbacks;
- request coroutines;
- response callbacks;
- pending write callbacks;
- timer callbacks.

Lifecycle must be:

```text
luaL_ref()
    ...
operation completes or is cancelled
    ...
luaL_unref()
```

Never retain:

- raw Lua stack indexes;
- unreferenced Lua functions;
- pointers to temporary Lua strings;
- Lua userdata after its owning object is destroyed.

Every cleanup path must release references, including:

- normal response completion;
- parser error;
- client disconnect;
- write failure;
- request timeout;
- server shutdown;
- Lua exception.

## 5. Request object

Expose a Lua request object/table containing:

```lua
{
    method = "GET",
    url = "/users?page=2",
    path = "/users",
    query = "page=2",
    version = "HTTP/1.1",
    headers = {
        ["host"] = "localhost",
        ["accept"] = "*/*"
    },
    body = "..."
}
```

Consider helper methods:

```lua
req:header("content-type")
req:query("page")
req:is_keep_alive()
req:remote_address()
```

Requirements:

- normalize header lookup consistently;
- decide whether duplicate headers become arrays or comma-separated strings;
- preserve raw URL if useful;
- separate path and query safely;
- support binary request bodies;
- do not assume the body is valid UTF-8;
- enforce body limits before allocating excessive memory.

## 6. Response object

Expose:

```lua
res:status(200)
res:header("Content-Type", "text/html")
res:send("hello")
```

Also consider:

```lua
res:json(value)
res:html(body)
res:redirect(location, status)
res:render(path, data)
res:write(chunk)
res:finish()
res:close()
```

For the first version, make `send()` the primary API and keep streaming optional.

Track response state:

```text
OPEN
HEADERS_SENT
WRITING
ENDED
ABORTED
```

Reject or safely handle:

```lua
res:send("one")
res:send("two")
```

```lua
res:send("body")
res:status(500)
```

```lua
res:close()
res:send("body")
```

Make double-send and send-after-close deterministic errors.

## 7. Response header validation

Validate outgoing header names and values.

Reject:

- carriage returns;
- line feeds;
- control characters;
- invalid header names;
- malformed header values.

Prevent:

- response splitting;
- conflicting `Content-Length`;
- invalid `Transfer-Encoding`;
- duplicate or contradictory connection headers.

Define behavior for:

- `Content-Type`;
- `Content-Length`;
- `Transfer-Encoding`;
- `Connection`;
- `Date`;
- `Server`;
- duplicate headers;
- cookies;
- empty responses;
- `HEAD` requests.

## 8. libuv write handling

Handle the fact that `uv_write()` is asynchronous.

Requirements:

- keep buffers alive until the write callback;
- do not free Lua strings prematurely;
- maintain a pending-write queue;
- handle write errors;
- handle client disconnect during writes;
- avoid unlimited queued output;
- stop accepting output after response completion;
- release buffers in every success/failure path.

For the first implementation, prefer:

```text
render entire response
construct complete HTTP response buffer
perform one uv_write()
```

This avoids unnecessary streaming complexity. Add chunked streaming later.

## 9. Backpressure

Implement a maximum pending output size per connection.

When the limit is reached:

- stop accepting more response data;
- pause or reject streaming;
- close the connection if necessary;
- return an error to Lua.

Do not allow a slow client to cause unlimited memory growth.

## 10. Client disconnect handling

When a client closes the connection:

- stop reads;
- cancel or invalidate the active request;
- prevent future writes;
- mark the response aborted;
- release Lua registry references;
- release buffers;
- close timers;
- destroy parser/request state;
- avoid resuming dead coroutines.

Test disconnects during:

- request body upload;
- template rendering;
- response writing;
- keep-alive idle time;
- large responses.

## 11. Request timeouts

Add configurable timers for:

- incomplete request headers;
- incomplete request body;
- idle keep-alive connections;
- request-handler execution;
- pending response writes;
- graceful shutdown.

Example configuration:

```lua
http.listen {
    host = "127.0.0.1",
    port = 8080,
    header_timeout = 10,
    body_timeout = 30,
    keepalive_timeout = 15,
    write_timeout = 30
}
```

## 12. Keep-alive support

Implement a deliberate policy for:

- HTTP/1.0;
- HTTP/1.1;
- `Connection: close`;
- `Connection: keep-alive`;
- idle timeout;
- multiple sequential requests;
- connection reuse after responses;
- parser reset between requests.

For the first release:

- support sequential keep-alive requests;
- do not support pipelining unless the response queue is explicitly designed for it;
- close the connection on parser or protocol errors.

## 13. `Expect: 100-continue`

Either:

- implement `100 Continue` correctly; or
- reject `Expect` requests clearly.

Do not leave clients waiting indefinitely.

## 14. Lua handler invocation

Define the handler API:

```lua
local http = require("http")

http.get("/", function(req, res)
    res:send("hello")
end)

http.post("/submit", function(req, res)
    print(req.body)
    res:status(201):send("created")
end)
```

Support:

- route registration;
- method matching;
- path matching;
- 404 responses;
- 405 responses;
- route parameters;
- query parsing;
- optional middleware later.

The C layer should invoke Lua handlers through protected calls or a protected coroutine boundary.

## 15. Lua error handling

If a handler or template throws:

```lua
error("something failed")
```

then:

- catch the Lua error;
- log it;
- return HTTP 500 if headers are not sent;
- close/abort if headers were already sent;
- release all request references;
- avoid crashing the entire process;
- preserve the server loop for future requests.

In development mode, optionally include error details in the response. In normal mode, return a generic 500 body.

Reuse the existing template line-mapping diagnostics for template errors.

## 16. Template rendering integration

Add an HTTP-facing renderer:

```lua
res:render("pages/index.tmg", data)
```

The implementation should:

- render through the existing TMG engine;
- provide request-specific template data;
- preserve shared application globals;
- avoid writing temporary output files;
- return/send the rendered result;
- convert rendering errors into request-level errors.

Do not make the template engine depend directly on HTTP. HTTP should consume the renderer.

## 17. Rendering output sink

The current renderer accumulates yielded output into `final_output`.

Initially, that is acceptable. Longer term, refactor rendering around an output callback/sink:

```c
typedef bool (*tmg_output_fn)(
    void *userdata,
    const char *data,
    size_t len
);
```

Potential sinks:

- string builder;
- file;
- HTTP response buffer;
- streaming response.

This allows future streaming without rewriting template execution.

## 18. Coroutine ownership

For each request that invokes Lua:

- create or use a request coroutine;
- keep it referenced;
- resume it only from the event-loop thread;
- detect `LUA_YIELD`;
- distinguish normal completion from errors;
- clean up after completion;
- never resume a completed or aborted coroutine;
- never resume a coroutine after its connection is destroyed.

If request handlers are initially synchronous, keep the coroutine design simple and add async yielding later.

## 19. Shared Lua global context

Keep the existing shared Lua state and global behavior.

Document the convention:

- application modules and configuration may use globals;
- request-local data should be stored in local variables or request tables;
- handlers should receive `req` and `res` explicitly;
- no automatic global request/response variables;
- shared globals are application-owned state.

This is an application coding convention, not a security boundary.

## 20. Path and template resolution

For static files or template rendering based on request paths:

- reject `..` path components;
- normalize `/` and platform separators;
- reject absolute paths where inappropriate;
- prevent document-root escape;
- define a template root;
- avoid blindly concatenating `req.path`;
- prefer registered routes;
- restrict template extensions if dynamic lookup is supported.

Prefer:

```lua
http.get("/", function(req, res)
    res:render("pages/index.tmg")
end)
```

over:

```lua
res:render("pages/" .. req.path .. ".tmg")
```

## 21. Static-file security

If static-file serving is added:

- resolve paths relative to a configured root;
- verify the resolved path remains inside that root;
- decide how symlinks are handled;
- prevent symlink escape;
- handle directories explicitly;
- prevent accidental source/template exposure;
- infer content types safely;
- support range requests only later;
- limit file size or use streaming for large files.

## 22. Resource limits

Make limits configurable for:

- maximum connections;
- maximum request line;
- maximum URL;
- maximum header bytes;
- maximum header count;
- maximum request body;
- maximum response size;
- maximum pending writes;
- maximum template size;
- maximum request duration;
- maximum keep-alive duration.

These protect the daemon from accidental or intentional resource exhaustion.

## 23. Blocking-operation policy

Document that handlers execute on the event-loop thread.

Blocking operations such as:

```lua
io.open(...)
io.popen(...)
os.execute(...)
```

will block other requests.

For the first version, this is acceptable. Later, add optional worker-pool APIs for:

- blocking filesystem work;
- subprocesses;
- database calls;
- CPU-heavy jobs.

Worker threads must never access the Lua state directly.

## 24. String utility hardening

Before exposing the renderer to arbitrary repeated requests, harden `str.c`:

- check `malloc()`;
- check `realloc()`;
- handle allocation overflow;
- handle zero/negative capacities safely;
- validate `ftell()` results;
- avoid `in_size * 2` overflow;
- use `size_t` for capacities and file sizes;
- handle `NULL` input safely;
- reset structures after deinitialization;
- define behavior for empty strings;
- avoid unchecked `strlen()` assumptions where binary data is possible.

This matters more for a long-running server than for a short build process.

## 25. Template cache

Consider a template cache with two modes:

```text
development:
    check/reload files on each request

production:
    cache compiled/generated templates
```

Handle:

- file modification checks;
- cache invalidation;
- cache size;
- stale templates;
- concurrent requests during reload;
- deleted templates;
- syntax errors during reload.

This is an optimization and can come after the first working server.

## 26. Graceful shutdown

Handle:

- SIGINT;
- SIGTERM;
- stop accepting new connections;
- allow active responses to finish;
- close idle keep-alive clients;
- close timers;
- close libuv handles;
- release Lua references;
- call `lua_close()` only after callbacks are impossible;
- exit with a meaningful status.

## 27. Logging

Add structured request/server logging with:

- timestamp;
- remote address;
- method;
- path;
- status;
- response bytes;
- duration;
- parser errors;
- handler errors;
- connection errors;
- active connection count.

Avoid logging bodies or sensitive headers by default.

Support at least:

```text
quiet
normal
debug
```

modes.

## 28. Build integration

Update the build system to support:

- libuv include paths;
- libuv library paths;
- llhttp sources or library;
- LuaJIT headers/library;
- Linux libraries such as `pthread`, `dl`, and possibly `rt`;
- Windows builds;
- macOS builds;
- static and shared linking;
- debug builds;
- sanitizer builds.

Prefer configurable variables:

```makefile
UV_CFLAGS
UV_LIBS
LLHTTP_CFLAGS
LLHTTP_LIBS
```

Decide whether libuv and llhttp are:

- vendored;
- Git submodules;
- system dependencies;
- selectable through Make variables.

## 29. Platform correctness

Test at least:

- Linux;
- macOS;
- Windows if supported.

Pay attention to:

- socket behavior;
- path separators;
- file APIs;
- signal handling;
- library naming;
- callback calling conventions;
- `size_t` and integer widths;
- libuv handle layout;
- filesystem encoding.

Avoid depending on private libuv struct layouts from LuaJIT FFI.

## 30. Testing

Add automated tests for:

### Parser behavior

- valid GET;
- POST body;
- chunked body;
- query strings;
- duplicate headers;
- malformed headers;
- oversized headers;
- invalid content length;
- incomplete requests;
- HTTP/1.0;
- HTTP/1.1.

### Connection behavior

- fragmented reads;
- keep-alive;
- connection close;
- client disconnect;
- slow client;
- multiple sequential requests;
- write failure;
- timeout;
- graceful shutdown.

### Lua behavior

- handler success;
- handler error;
- template success;
- template syntax error;
- template runtime error;
- callback garbage collection;
- multiple concurrent requests;
- cleanup after aborted requests.

### Response behavior

- status codes;
- headers;
- content length;
- empty responses;
- binary bodies;
- double-send;
- send-after-close;
- partial write handling.

## 31. Fuzzing

Fuzz:

- llhttp input;
- request-to-Lua conversion;
- headers;
- URL/path handling;
- body size boundaries;
- malformed requests;
- connection teardown during parsing.

The goal is to find:

- crashes;
- use-after-free;
- double-free;
- parser desynchronization;
- unbounded allocation;
- invalid Lua stack operations.

## 32. Sanitizer/debug builds

Add development targets for:

```text
-fsanitize=address
-fsanitize=undefined
-fsanitize=leak
-g
-DLUA_USE_APICHECK
```

Use them for:

- connection teardown;
- callback cleanup;
- write callbacks;
- coroutine lifecycle;
- parser failures;
- repeated requests.

## 33. HTTP features to postpone

Do not block the first implementation on:

- HTTP/2;
- HTTP/3;
- WebSockets;
- TLS;
- compression;
- multipart parsing;
- range requests;
- advanced middleware;
- hot reload;
- distributed sessions;
- clustering;
- worker-process supervision.

TLS can initially be handled by deploying behind a reverse proxy such as Caddy or nginx.

# Suggested implementation phases

## Phase 1: Minimum working HTTP server

- libuv TCP server;
- llhttp parser;
- one Lua handler;
- request object;
- response `status/header/send`;
- one complete buffered response;
- basic error handling;
- basic connection cleanup.

## Phase 2: Correctness and lifecycle

- registry references;
- disconnect handling;
- write callbacks;
- keep-alive;
- timeouts;
- response state machine;
- request/body/header limits;
- graceful shutdown.

## Phase 3: TMG integration

- `res:render()`;
- template error conversion;
- request data passed to templates;
- binary-safe response handling;
- optional output sink abstraction.

## Phase 4: Routing and usability

- route methods;
- path parameters;
- query helpers;
- JSON responses;
- redirects;
- static files;
- middleware.

## Phase 5: Hardening and performance

- template cache;
- sanitizer builds;
- fuzzing;
- benchmarks;
- logging;
- backpressure improvements;
- optional streaming;
- worker-pool APIs.
