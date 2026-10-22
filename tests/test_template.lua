local function check(cond, msg)
    if not cond then error(msg or "check failed", 2) end
end

local hash_path = "tests/fixtures/templates-sep/hash.tmg"
local at_path = "tests/fixtures/templates-sep/at.tmg"
local hash_static_path = "tests/fixtures/templates-sep/hash_static.tmg"
local at_static_path = "tests/fixtures/templates-sep/at_static.tmg"
local hello_path = "tests/fixtures/templates/hello.tmg"

local function rendered(path, sep, data)
    local out, err = tmg.render_string(path, sep, data)
    check(out ~= nil, err or "render_string failed")
    return out
end

tmg_delim = "%"
local hello = rendered(hello_path, nil, { name = "Ada" })
check(hello:find("Hello Ada!", 1, true), "default percent delimiter")

tmg_delim = "%"
local hash = rendered(hash_path, "#", "Ada")
check(hash:find("Hash: Ada", 1, true), "explicit hash delimiter")
check(hash:find("percent: 100%", 1, true), "percent should stay literal")
check(hash:find("at: @@ literal", 1, true), "at sign should stay literal")
check(not hash:find("# tmg.print", 1, true), "hash code block was not parsed")

tmg_delim = "@@"
local at = rendered(at_path, nil, "Bob")
check(at:find("At: Bob", 1, true), "global at delimiter")
check(at:find("percent: 100%", 1, true), "percent should stay literal")
check(at:find("hash: # literal", 1, true), "hash should stay literal")
check(not at:find("@@ tmg.print", 1, true), "at code block was not parsed")

tmg_delim = "@@"
local overridden = rendered(hash_path, "#", "Cara")
check(overridden:find("Hash: Cara", 1, true),
      "explicit delimiter should override tmg_delim")

tmg_delim = "#"
local explicit_at = rendered(at_path, "@@", "Dan")
check(explicit_at:find("At: Dan", 1, true),
      "explicit multi-character delimiter")

local out_path = os.tmpname()
local ok = tmg.render(hash_static_path, out_path, "#")
check(ok, "tmg.render explicit delimiter failed")
local f = assert(io.open(out_path, "rb"))
local body = f:read("*a")
f:close()
os.remove(out_path)
check(body:find("Hash explicit: explicit", 1, true),
      "tmg.render output missing hash value")

tmg_delim = "@@"
ok = tmg.render(at_static_path, out_path)
check(ok, "tmg.render global delimiter failed")
f = assert(io.open(out_path, "rb"))
body = f:read("*a")
f:close()
os.remove(out_path)
check(body:find("At global: global", 1, true),
      "tmg.render output missing at value")

print("ok: template separator tests")
