# TMG
a Lua runtime with templating built in. Write a `build.lua` script that
calls into `tmg.render(...)` to expand `.tmg` templates — the templates
themselves still use `lua` to script the parts that expand.

## Integration
Generate base16 colors using wallpaper with [tmg-wall](https://github.com/commrade-goad/tmg-wall).

## Build
1. __Dependency:__
    * `lua`

2. __Build:__

```sh
make all -j$(nproc)
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
embedded Lua (default `%`); use `$` right after opening a code block to
print an expression inline (`%$expr%`). See `example/` for templates.

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
