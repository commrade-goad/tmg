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
