-- latex.lua: build LaTeX from Lua tables.
--
--   local L = require("latex")
--   local doc = L.document{ title = "Hi", L.section("Intro", "text") }
--   L.write(doc, "out.tex")        -- or L.print(node) inside a .tmg template
--
-- Rules of thumb:
--   * a Lua string child is TEXT and gets escaped; use L.raw"..." for LaTeX
--   * options ([..]) and math strings are raw LaTeX
--   * a plain table child is a list and is flattened into its parent
--   * every node renders to a string: tostring(node) == L.render(node)

local L = {}

local unpack = table.unpack or unpack

---------------------------------------------------------------------------
-- nodes & helpers
---------------------------------------------------------------------------

local NODE = {}                       -- metatable shared by all nodes

local function is_node(x) return getmetatable(x) == NODE end
local function is_list(x) return type(x) == "table" and getmetatable(x) == nil end

local function mk(kind, t)
    t._t = kind
    return setmetatable(t, NODE)
end

local function copy(t)
    local r = {}
    for k, v in pairs(t) do r[k] = v end
    return r
end

local function check_holes(t, n, who)
    for i = 1, n do
        if t[i] == nil then
            error(("latex: %s: child #%d is nil"):format(who, i), 3)
        end
    end
end

local function pack(who, ...)
    local t = { ... }
    check_holes(t, select("#", ...), who)
    return t
end

local ESC = {
    ["\\"] = "\\textbackslash{}", ["&"] = "\\&", ["%"] = "\\%", ["$"] = "\\$",
    ["#"] = "\\#", ["_"] = "\\_", ["{"] = "\\{", ["}"] = "\\}",
    ["~"] = "\\textasciitilde{}", ["^"] = "\\textasciicircum{}",
}

local function escape(s) return (s:gsub("[\\&%%$#_{}~^]", ESC)) end
L.escape = escape

-- "[a,b,k=v]" from a raw string or a table (array part first, then k=v sorted)
local function fmt_opts(o)
    if o == nil or o == false then return "" end
    if type(o) == "string" then return o == "" and "" or ("[" .. o .. "]") end
    local parts = {}
    for _, v in ipairs(o) do parts[#parts + 1] = tostring(v) end
    local keys = {}
    for k in pairs(o) do
        if type(k) == "string" then keys[#keys + 1] = k end
    end
    table.sort(keys)
    for _, k in ipairs(keys) do
        local v = o[k]
        parts[#parts + 1] = (v == true) and k or (k .. "=" .. tostring(v))
    end
    if #parts == 0 then return "" end
    return "[" .. table.concat(parts, ",") .. "]"
end

---------------------------------------------------------------------------
-- rendering core
---------------------------------------------------------------------------

local R = {}                          -- kind -> function(node, ctx) -> str, is_block

local function pad(ctx) return ctx.indent and ("  "):rep(ctx.depth) or "" end
local function deeper(ctx, k) return { indent = ctx.indent, depth = ctx.depth + (k or 1) } end

local function flatten(list, out, who)
    for i = 1, table.maxn(list) do
        local c = list[i]
        if c == nil then
            error(("latex: %s: child #%d is nil"):format(who, i), 0)
        end
        if is_list(c) then flatten(c, out, who) else out[#out + 1] = c end
    end
    return out
end

local function render_one(c, ctx)
    local tc = type(c)
    if tc == "string" then return escape(c), false end
    if tc == "number" then return tostring(c), false end
    if is_node(c) then
        local f = R[c._t]
        if not f then error("latex: unknown node kind '" .. tostring(c._t) .. "'", 0) end
        local s, blk = f(c, ctx)
        return s, blk and true or false
    end
    error("latex: cannot render a " .. tc .. " value", 0)
end

-- Renders the positional children of `children`. Inline pieces are glued
-- together; a block piece is separated from its neighbours by `bsep`.
local function render_children(children, ctx, who, bsep)
    bsep = bsep or "\n\n"
    local items = flatten(children, {}, who)
    local out, prev_block, any_block = {}, false, false
    for _, c in ipairs(items) do
        local s, blk = render_one(c, ctx)
        if blk or s ~= "" then
            if #out > 0 and (blk or prev_block) then out[#out + 1] = bsep end
            out[#out + 1] = s
            prev_block = blk
            any_block = any_block or blk
        end
    end
    return table.concat(out), any_block
end

-- render one child (string / node / list) as inline text
local function inline(x, ctx, who)
    return (render_children({ x }, ctx, who))
end

---------------------------------------------------------------------------
-- raw, fragments, paragraphs, generic commands & environments
---------------------------------------------------------------------------

R.raw = function(n) return n[1], n.block == true end

function L.raw(s)      return mk("raw", { tostring(s) }) end
function L.rawblock(s) return mk("raw", { tostring(s), block = true }) end

R.frag = function(n, ctx) return render_children(n, ctx, "frag") end
function L.frag(...) return mk("frag", pack("frag", ...)) end

R.par = function(n, ctx) return (render_children(n, ctx, "par")), true end
function L.par(...) return mk("par", pack("par", ...)) end

local function render_args(args, ctx, who)
    local out = {}
    for _, a in ipairs(args) do
        out[#out + 1] = "{" .. inline(a, ctx, who) .. "}"
    end
    return table.concat(out)
end

R.cmd = function(n, ctx)
    return "\\" .. n.name .. fmt_opts(n.opts) .. render_args(n, ctx, n.name), false
end

-- L.cmd("name", {opts}, arg1, arg2, ...)  ->  \name[opts]{arg1}{arg2}
function L.cmd(name, opts, ...)
    assert(type(name) == "string", "latex: cmd name must be a string")
    local t = pack("cmd " .. name, ...)
    t.name, t.opts = name, opts
    return mk("cmd", t)
end

local function cmd1(name)
    return function(...) return L.cmd(name, nil, ...) end
end

-- mandatory environment arguments are layout values ("0.5\\textwidth", "lrr",
-- "2"), so plain strings are raw LaTeX here; nodes still render normally
local function env_args(args, ctx, who)
    local out = {}
    for _, a in ipairs(args or {}) do
        out[#out + 1] = "{" .. (type(a) == "string" and a or inline(a, ctx, who)) .. "}"
    end
    return table.concat(out)
end

R.env = function(n, ctx)
    -- an env that directly contains \item nodes is laid out like a list
    local has_items = false
    for _, c in ipairs(flatten(n, {}, n.name)) do
        if is_node(c) and c._t == "item" then has_items = true; break end
    end
    local p = has_items and pad(ctx) or ""
    local bctx, bsep = ctx, "\n\n"
    if has_items then bctx, bsep = deeper(ctx), "\n" end

    local s = p .. "\\begin{" .. n.name .. "}" .. fmt_opts(n.opts) .. env_args(n.args, ctx, n.name)
    local body = render_children(n, bctx, n.name, bsep)
    if body ~= "" then s = s .. "\n" .. body end
    return s .. "\n" .. p .. "\\end{" .. n.name .. "}", true
end

-- the second argument of env is `spec`; everything in it is optional:
--   "htbp"                        -> [htbp]
--   { "t", width = "5cm" }        -> [t,width=5cm]       (options, like L.cmd)
--   { "t", args = { "2cm" } }     -> [t]{2cm}            (`args` = mandatory arguments)
--   { opts = "t", args = {..} }   -> same, spelled out
local function split_spec(spec)
    if spec == nil then return nil, nil end
    if type(spec) == "string" then return spec, nil end
    if type(spec) ~= "table" then
        error("latex: env: spec must be a string or a table, got " .. type(spec), 3)
    end
    local args = spec.args
    if args ~= nil and type(args) ~= "table" then args = { args } end
    local opts = spec.opts
    if opts == nil then
        local o = {}
        for i = 1, table.maxn(spec) do o[i] = spec[i] end
        for k, v in pairs(spec) do
            if type(k) == "string" and k ~= "args" and k ~= "opts" then o[k] = v end
        end
        if next(o) ~= nil then opts = o end
    end
    return opts, args
end

-- L.env("name", spec, ...children)
-- L.env{ "name", opts = .., args = {..}, ...children }      (same thing, one table)
function L.env(name, spec, ...)
    local t
    if is_list(name) then
        local cfg = name
        if type(cfg[1]) ~= "string" then
            error("latex: env: the first entry of the table must be the environment name", 2)
        end
        t = {}
        for i = 2, table.maxn(cfg) do t[i - 1] = cfg[i] end
        check_holes(t, table.maxn(cfg) - 1, "env " .. cfg[1])
        name, spec = cfg[1], { opts = cfg.opts, args = cfg.args }
    else
        assert(type(name) == "string", "latex: env name must be a string")
        t = pack("env " .. name, ...)
    end
    t.name = name
    t.opts, t.args = split_spec(spec)
    return mk("env", t)
end

-- \item as a node: L.item("text"), L.item{ term = "Foo", "text" }.
-- Inside L.env("itemize", ...) / L.itemize{} every item gets its own line.
R.item = function(n, ctx)
    local term = n.term and ("[" .. inline(n.term, ctx, "item") .. "]") or ""
    local body = render_children(n, deeper(ctx), "item", "\n"):gsub("^%s+", "")
    return pad(ctx) .. "\\item" .. term .. (body ~= "" and (" " .. body) or ""), true
end

function L.item(...)
    local first = ...
    local t
    if select("#", ...) == 1 and is_list(first) and first.term ~= nil then
        t = copy(first)
        check_holes(t, table.maxn(t), "item")
    else
        t = pack("item", ...)
    end
    return mk("item", t)
end

-- dynamic sugar: L.c.textbf"x", L.e.center{ ... }
-- (for envs a single table with `opts`/`args` fields carries the spec:
--  L.e.minipage{ opts = "t", args = { "0.5\\textwidth" }, "text" })
L.c = setmetatable({}, { __index = function(_, name) return cmd1(name) end })
L.e = setmetatable({}, { __index = function(_, name)
    return function(...)
        local first = ...
        if select("#", ...) == 1 and is_list(first) and (first.opts ~= nil or first.args ~= nil) then
            return L.env(name, { opts = first.opts, args = first.args },
                         unpack(first, 1, table.maxn(first)))
        end
        return L.env(name, nil, ...)
    end
end })

---------------------------------------------------------------------------
-- text helpers
---------------------------------------------------------------------------

-- text helpers: ALL arguments are the children of one {group}
local function group1(name)
    return function(...) return L.cmd(name, nil, { ... }) end
end

L.bf        = group1("textbf")
L.it        = group1("textit")
L.tt        = group1("texttt")
L.emph      = group1("emph")
L.sc        = group1("textsc")
L.underline = group1("underline")
L.footnote  = group1("footnote")

local function rawkeys(...) return L.raw(table.concat({ ... }, ",")) end

function L.label(key)  return L.cmd("label",  nil, L.raw(key)) end
function L.ref(key)    return L.cmd("ref",    nil, L.raw(key)) end
function L.eqref(key)  return L.cmd("eqref",  nil, L.raw(key)) end
function L.cite(...)   return L.cmd("cite",   nil, rawkeys(...)) end
function L.url(u)      return L.cmd("url",    nil, L.raw(u)) end
function L.href(u, ...) return L.cmd("href",  nil, L.raw(u), { ... }) end

-- inline math: L.m"E = mc^2" -> $E = mc^2$   (raw LaTeX)
R.m = function(n) return "$" .. n[1] .. "$", false end
function L.m(...)
    local parts = {}
    for i = 1, select("#", ...) do parts[i] = tostring((select(i, ...))) end
    return mk("m", { table.concat(parts) })
end

L.newpage         = mk("raw", { "\\newpage",         block = true })
L.clearpage       = mk("raw", { "\\clearpage",       block = true })
L.tableofcontents = mk("raw", { "\\tableofcontents", block = true })
L.maketitle       = mk("raw", { "\\maketitle",       block = true })

R.newcommand = function(n)
    return "\\newcommand{\\" .. n.name .. "}"
           .. (n.nargs and ("[" .. n.nargs .. "]") or "")
           .. "{" .. n.def .. "}", true
end

-- L.newcommand("R", "\\mathbb{R}") / L.newcommand("vec", "\\mathbf{#1}", 1)
function L.newcommand(name, def, nargs)
    return mk("newcommand", { name = name:gsub("^\\", ""), def = def, nargs = nargs })
end

---------------------------------------------------------------------------
-- sectioning
---------------------------------------------------------------------------

R.sec = function(n, ctx)
    local head = "\\" .. n.cmd .. (n.star and "*" or "")
                 .. (n.short and ("[" .. escape(n.short) .. "]") or "")
                 .. "{" .. (render_children(n.title, ctx, n.cmd)) .. "}"
    if n.label then head = head .. "\n\\label{" .. n.label .. "}" end
    local body = render_children(n, ctx, n.cmd)
    if body ~= "" then head = head .. "\n\n" .. body end
    return head, true
end

-- L.section("Title", ...children)
-- L.section({ "Title", label = "sec:x", star = true, short = "T" }, ...children)
local function sectioning(cmdname)
    return function(title, ...)
        if title == nil then error("latex: " .. cmdname .. ": title is required", 2) end
        local t = pack(cmdname, ...)
        t.cmd = cmdname
        if is_list(title) then
            t.title, t.label, t.star, t.short = title, title.label, title.star, title.short
        else
            t.title = { title }
        end
        return mk("sec", t)
    end
end

for _, name in ipairs{ "part", "chapter", "section", "subsection", "subsubsection",
                       "paragraph", "subparagraph" } do
    L[name] = sectioning(name)
end

---------------------------------------------------------------------------
-- lists
---------------------------------------------------------------------------

R.list = function(n, ctx)
    local p, p1 = pad(ctx), pad(deeper(ctx))
    local items = {}
    for i = 1, table.maxn(n) do
        local it = n[i]
        if it == nil then
            error(("latex: %s: item #%d is nil"):format(n.env, i), 0)
        end
        if is_node(it) and it._t == "list" and #items > 0 then
            -- a nested list belongs to the previous item
            local parts = items[#items].parts
            parts[#parts + 1] = it
        elseif is_list(it) or (is_node(it) and it._t == "item") then
            items[#items + 1] = { term = it.term, parts = { unpack(it, 1, table.maxn(it)) } }
        else
            items[#items + 1] = { parts = { it } }
        end
    end

    local lines = { p .. "\\begin{" .. n.env .. "}" .. fmt_opts(n.opts) }
    local ictx = deeper(ctx, 2)
    for _, it in ipairs(items) do
        local term = it.term and ("[" .. inline(it.term, ctx, n.env) .. "]") or ""
        local body = render_children(it.parts, ictx, n.env, "\n"):gsub("^%s+", "")
        lines[#lines + 1] = p1 .. "\\item" .. term .. (body ~= "" and (" " .. body) or "")
    end
    lines[#lines + 1] = p .. "\\end{" .. n.env .. "}"
    return table.concat(lines, "\n"), true
end

-- L.itemize{ "a", "b", L.enumerate{ "nested under b" }, opts = "noitemsep" }
-- an item may be a list of parts: { "text", L.m"x" }, or { term = "T", "text" }
local function listkind(env)
    return function(cfg)
        local t = copy(cfg)
        check_holes(t, table.maxn(t), env)
        t.env = env
        return mk("list", t)
    end
end

L.itemize, L.enumerate, L.description =
    listkind("itemize"), listkind("enumerate"), listkind("description")

---------------------------------------------------------------------------
-- math environments, verbatim, listings
---------------------------------------------------------------------------

R.mathenv = function(n, ctx)
    local p = pad(ctx)
    local env = n.env .. (n.star and "*" or "")
    local lines = {}
    for i = 1, table.maxn(n) do lines[i] = p .. "  " .. tostring(n[i]) end
    if n.label then
        if n.env == "equation" or #lines == 0 then
            table.insert(lines, 1, p .. "  \\label{" .. n.label .. "}")
        else
            lines[1] = lines[1] .. " \\label{" .. n.label .. "}"
        end
    end
    local sep = n.env == "equation" and "\n" or " \\\\\n"
    return p .. "\\begin{" .. env .. "}\n" .. table.concat(lines, sep)
           .. "\n" .. p .. "\\end{" .. env .. "}", true
end

-- L.eq"E = mc^2"  /  L.eq{ "E = mc^2", label = "eq:e", star = true }
-- L.align{ "a &= b", "c &= d" }       (lines are raw LaTeX)
local function mathenv(env)
    return function(cfg)
        local t = type(cfg) == "table" and copy(cfg) or { cfg }
        check_holes(t, table.maxn(t), env)
        t.env = env
        return mk("mathenv", t)
    end
end

L.eq, L.align, L.gather = mathenv("equation"), mathenv("align"), mathenv("gather")

local function verb_check(s, name)
    if s:find("\\end{" .. name .. "}", 1, true) then
        error("latex: " .. name .. " body contains \\end{" .. name .. "}", 3)
    end
end

R.verbatim = function(n)
    return "\\begin{verbatim}\n" .. n[1] .. "\n\\end{verbatim}", true
end
function L.verbatim(s)
    s = tostring(s); verb_check(s, "verbatim")
    return mk("verbatim", { s })
end

R.lstlisting = function(n)
    return "\\begin{lstlisting}" .. fmt_opts(n.opts) .. "\n" .. n[1]
           .. "\n\\end{lstlisting}", true
end
-- L.lstlisting(code, { language = "Python", caption = "..." })   (needs \usepackage{listings})
function L.lstlisting(code, opts)
    code = tostring(code); verb_check(code, "lstlisting")
    return mk("lstlisting", { code, opts = opts })
end

---------------------------------------------------------------------------
-- tables
---------------------------------------------------------------------------

L.toprule    = mk("rule", { cmd = "\\toprule" })
L.midrule    = mk("rule", { cmd = "\\midrule" })
L.bottomrule = mk("rule", { cmd = "\\bottomrule" })
L.hline      = mk("rule", { cmd = "\\hline" })
R.rule = function(n) return n.cmd, true end

-- L.multicolumn(2, "c", "text") for use as a cell
function L.multicolumn(ncols, spec, ...)
    return L.cmd("multicolumn", nil, tostring(ncols), L.raw(spec), { ... })
end

local function row_str(cells, ctx, what)
    if not is_list(cells) then
        error("latex: table: " .. what .. " must be a list of cells", 0)
    end
    local out = {}
    for i = 1, table.maxn(cells) do
        if cells[i] == nil then
            error(("latex: table: nil cell #%d in %s"):format(i, what), 0)
        end
        out[i] = inline(cells[i], ctx, "cell")
    end
    return table.concat(out, " & ") .. " \\\\"
end

-- the \begin{tabular} ... \end{tabular} lines, padded for `ctx`
local function tabular_lines(cfg, ctx)
    local p = pad(ctx)
    if cfg.resize then
        local inner = tabular_lines({
            cols = cfg.cols, header = cfg.header, rows = cfg.rows, booktabs = cfg.booktabs,
        }, deeper(ctx))
        return p .. "\\resizebox{\\textwidth}{!}{%\n" .. inner .. "\n" .. p .. "}"
    end

    local p1 = pad(deeper(ctx))
    local booktabs = cfg.booktabs ~= false
    local top, mid, bot
    if booktabs then top, mid, bot = "\\toprule", "\\midrule", "\\bottomrule"
    else top, mid, bot = "\\hline", "\\hline", "\\hline" end

    local ncols
    if cfg.header then
        ncols = table.maxn(cfg.header)
    else
        for _, r in ipairs(cfg.rows or {}) do
            if is_list(r) then ncols = table.maxn(r); break end
        end
    end
    local cols = cfg.cols or (("l"):rep(ncols or 1))

    local lines = { p .. "\\begin{tabular}{" .. cols .. "}", p1 .. top }
    if cfg.header then
        lines[#lines + 1] = p1 .. row_str(cfg.header, ctx, "header")
        lines[#lines + 1] = p1 .. mid
    end
    local rows = cfg.rows or {}
    for i = 1, table.maxn(rows) do
        local r = rows[i]
        if r == nil then error(("latex: table: row #%d is nil"):format(i), 0) end
        if is_node(r) and r._t == "rule" then
            lines[#lines + 1] = p1 .. r.cmd
        else
            lines[#lines + 1] = p1 .. row_str(r, ctx, "row #" .. i)
        end
    end
    lines[#lines + 1] = p1 .. bot
    lines[#lines + 1] = p .. "\\end{tabular}"
    return table.concat(lines, "\n")
end

R.tabular = function(n, ctx) return tabular_lines(n, ctx), true end

-- L.tabular{ cols = "lrr", header = {...}, rows = { {...}, ... },
--            booktabs = true, resize = false }
function L.tabular(cfg) return mk("tabular", copy(cfg)) end

local function float_lines(env, n, ctx, body_lines, default_pos, default_cap)
    local p, p1 = pad(ctx), pad(deeper(ctx))
    local pos = n.pos == nil and default_pos or n.pos
    local lines = { p .. "\\begin{" .. env .. "}" .. (pos and ("[" .. pos .. "]") or "") }
    if n.center ~= false then lines[#lines + 1] = p1 .. "\\centering" end

    local cap = {}
    if n.caption then
        cap[#cap + 1] = p1 .. "\\caption{" .. inline(n.caption, ctx, env) .. "}"
    end
    if n.label then cap[#cap + 1] = p1 .. "\\label{" .. n.label .. "}" end

    local pos_cap = n.caption_pos or default_cap
    if pos_cap == "top" then for _, l in ipairs(cap) do lines[#lines + 1] = l end end
    for _, l in ipairs(body_lines) do lines[#lines + 1] = l end
    if pos_cap ~= "top" then for _, l in ipairs(cap) do lines[#lines + 1] = l end end

    lines[#lines + 1] = p .. "\\end{" .. env .. "}"
    return table.concat(lines, "\n")
end

R.table = function(n, ctx)
    return float_lines("table", n, ctx, { tabular_lines(n, deeper(ctx)) }, "htbp", "top"), true
end

-- L.table{ caption = "..", label = "tab:x", cols = "lrr", header = {..}, rows = {..},
--          pos = "htbp", center = true, caption_pos = "top", resize = false }
function L.table(cfg) return mk("table", copy(cfg)) end

---------------------------------------------------------------------------
-- figures
---------------------------------------------------------------------------

R.figure = function(n, ctx)
    local w = n.width
    if type(w) == "number" then w = ("%g\\textwidth"):format(w) end
    local o = {}
    if w then o[#o + 1] = "width=" .. w end
    if n.opts then o[#o + 1] = n.opts end
    local line = pad(deeper(ctx)) .. "\\includegraphics"
                 .. (#o > 0 and ("[" .. table.concat(o, ",") .. "]") or "")
                 .. "{" .. n.file .. "}"
    return float_lines("figure", n, ctx, { line }, "htbp", "bottom"), true
end

-- L.figure{ file = "plot.pdf", width = 0.8, caption = "..", label = "fig:x" }
function L.figure(cfg)
    assert(type(cfg) == "table" and cfg.file, "latex: figure needs a `file`")
    return mk("figure", copy(cfg))
end

---------------------------------------------------------------------------
-- document
---------------------------------------------------------------------------

R.document = function(n, ctx)
    local head = {}
    local function add(s) head[#head + 1] = s end

    add("\\documentclass" .. fmt_opts(n.opts) .. "{" .. (n.class or "article") .. "}")
    for _, pk in ipairs(n.packages or {}) do
        if type(pk) == "string" then
            add("\\usepackage{" .. pk .. "}")
        else
            local o = {}
            for i = 2, #pk do o[#o + 1] = pk[i] end
            add("\\usepackage" .. fmt_opts(o) .. "{" .. pk[1] .. "}")
        end
    end
    for _, item in ipairs(n.preamble or {}) do
        if type(item) == "string" then add(item) else add((render_one(item, ctx))) end
    end
    if n.title  then add("\\title{"  .. inline(n.title,  ctx, "title")  .. "}") end
    if n.author then add("\\author{" .. inline(n.author, ctx, "author") .. "}") end
    if n.date == false then
        add("\\date{}")
    elseif n.date ~= nil then
        add("\\date{" .. inline(n.date, ctx, "date") .. "}")
    end

    local parts = {}
    if n.title and n.maketitle ~= false then parts[#parts + 1] = "\\maketitle" end
    if n.toc then parts[#parts + 1] = "\\tableofcontents" end
    local body = render_children(n, ctx, "document")
    if body ~= "" then parts[#parts + 1] = body end
    if n.bib then
        local f = n.bib.file
        if type(f) == "table" then f = table.concat(f, ",") end
        parts[#parts + 1] = "\\bibliographystyle{" .. (n.bib.style or "plain") .. "}\n"
                            .. "\\bibliography{" .. f .. "}"
    end

    local s = table.concat(head, "\n") .. "\n\n\\begin{document}"
    if #parts > 0 then s = s .. "\n\n" .. table.concat(parts, "\n\n") end
    return s .. "\n\n\\end{document}", true
end

-- L.document{ class, opts, packages = { "amsmath", {"geometry","margin=2cm"} },
--             preamble = { L.newcommand(..), "\\raw line" }, title, author, date,
--             maketitle = true, toc = false, bib = { style, file }, ...children }
function L.document(cfg)
    cfg = copy(cfg or {})
    check_holes(cfg, table.maxn(cfg), "document")
    return mk("document", cfg)
end

---------------------------------------------------------------------------
-- output
---------------------------------------------------------------------------

NODE.__tostring = function(n) return L.render(n) end
NODE.__concat   = function(a, b) return tostring(a) .. tostring(b) end

-- L.render(node | string | list [, { indent = true }]) -> string (no trailing newline)
function L.render(x, o)
    o = o or {}
    local ctx = { indent = o.indent ~= false, depth = 0 }
    return (render_children({ x }, ctx, "render"))
end

-- L.write(node, path [, opts]) -> true | nil, err
function L.write(x, path, o)
    local s = L.render(x, o)
    local f, err = io.open(path, "w")
    if not f then return nil, err end
    f:write(s, "\n")
    f:close()
    return true
end

-- Prints into a .tmg template (through tmg.print). Outside of a template
-- (e.g. directly in build.lua) it writes to stdout instead.
function L.print(...)
    for i = 1, select("#", ...) do
        local s = L.render((select(i, ...)))
        local co, ismain = coroutine.running()
        if type(tmg) == "table" and tmg.print and co and not ismain then
            tmg.print(s)
        else
            io.write(s)
        end
    end
end

local function shq(s) return "'" .. (tostring(s):gsub("'", "'\\''")) .. "'" end

-- L.compile("out.tex" [, { engine = "latexmk", args = { ... } }]) -> ok, log_tail
function L.compile(path, o)
    o = o or {}
    local engine = o.engine or "latexmk"
    local dir, file = path:match("^(.*)/([^/]*)$")
    if not dir then dir, file = ".", path end
    if dir == "" then dir = "/" end

    local args = { "-interaction=nonstopmode", "-halt-on-error" }
    if engine == "latexmk" then table.insert(args, 1, "-pdf") end
    for _, a in ipairs(o.args or {}) do args[#args + 1] = a end
    for i, a in ipairs(args) do args[i] = shq(a) end

    local log = os.tmpname()
    local r = os.execute(("cd %s && %s %s %s > %s 2>&1"):format(
        shq(dir), shq(engine), table.concat(args, " "), shq(file), shq(log)))
    local ok = (r == 0 or r == true)

    local f = io.open(log, "r")
    local text = f and f:read("*a") or ""
    if f then f:close() end
    os.remove(log)
    if ok then return true, "" end

    -- prefer the TeX errors ("! ..." plus context) from the .log file: they
    -- survive a latexmk run that did nothing because it saw an earlier error
    local lf = io.open(dir .. "/" .. file:gsub("%.[^.]*$", "") .. ".log", "r")
    if lf then
        local errs, ll = {}, {}
        for l in lf:lines() do ll[#ll + 1] = l end
        lf:close()
        for i, l in ipairs(ll) do
            if l:sub(1, 1) == "!" then
                errs[#errs + 1] = table.concat(ll, "\n", i, math.min(#ll, i + 2))
            end
        end
        if #errs > 0 then return false, table.concat(errs, "\n") end
    end

    local lines = {}
    for l in text:gmatch("[^\n]+") do lines[#lines + 1] = l end
    return false, table.concat(lines, "\n", math.max(1, #lines - 29))
end

return L
