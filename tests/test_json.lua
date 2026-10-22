local http = require("http")

local function check(cond, msg)
    if not cond then error(msg or "check failed", 2) end
end

local function encode(value, opts)
    local out, err = http.json_encode(value, opts)
    check(out, err)
    return out
end

local function decode(text, opts)
    local value, err = http.json_decode(text, opts)
    check(err == nil, err)
    return value
end

check(encode({ 1, "a", true }) == '[1,"a",true]', "array encode")
check(encode({}) == "{}", "empty table encode")
check(encode(nil) == "null", "nil encode")
check(encode('a"b\\c\n') == '"a\\"b\\\\c\\n"', "string escape encode")
check(encode(1e2) == "100", "number encode")
check(encode({ a = 1 }):find('"a":1', 1, true) ~= nil, "object encode")

local round = decode('[1,2,{"x":[true,false,null,null]}]')
check(round[1] == 1 and round[2] == 2, "array decode")
check(round[3].x[1] == true and round[3].x[2] == false, "nested decode")
check(round[3].x[3] == nil and round[3].x[4] == nil, "null decode")

local nul = decode('{"a":null,"b":1}')
check(nul.a == nil and nul.b == 1, "object null decode")

local unicode = decode('"\\u00e9\\ud83d\\ude00\\u0000"')
check(unicode == "\195\169\240\159\152\128\0", "unicode decode")

local escapes = decode('"\\\"\\\\\\/\\b\\f\\n\\r\\t"')
check(escapes == "\"\\/\b\f\n\r\t", "escape decode")
check(decode("1e2") == 100, "exponent decode")
check(decode("-0.5") == -0.5, "fraction decode")
check(decode("1234567890123") == 1234567890123, "integer decode")

local dup = decode('{"a":1,"a":2}')
check(dup.a == 2, "duplicate key should use last value")

local big = string.rep("x", 2 * 1024 * 1024)
local big_json = encode(big)
check(#big_json == #big + 2, "large encode truncated")
check(decode(big_json) == big, "large round trip")

local nested = {}
local cur = nested
for i = 1, 10 do
    cur.next = {}
    cur = cur.next
end
check(not http.json_encode(nested, { max_depth = 3 }), "encode depth limit")
local deep_text = string.rep("[", 10) .. string.rep("]", 10)
local deep_value, deep_err = http.json_decode(deep_text, { max_depth = 3 })
check(deep_value == nil and deep_err, "decode depth limit")

local cycle = {}
cycle.self = cycle
local cycle_json, cycle_err = http.json_encode(cycle)
check(cycle_json == nil and cycle_err, "encode cycle")

local invalid = {
    "",
    " ",
    "01",
    "1.",
    "1e",
    "-",
    ".1",
    "NaN",
    "tru",
    "[] trailing",
    "[1,]",
    '{"a":1,}',
    '{"a"}',
    '"unterminated',
    '"\\q"',
    '"\\u12"',
    '"\\ud800"',
    '"\\udc00"',
    '"a\nb"',
    "nullx",
}
for _, text in ipairs(invalid) do
    local value, err = http.json_decode(text)
    check(value == nil and err, "invalid JSON accepted: " .. text)
end

local bad, bad_err = http.json_encode("x", { max_depth = 0 })
check(bad == nil and bad_err, "invalid encode depth option")
local bad2, bad2_err = http.json_decode("1", { max_depth = 0 })
check(bad2 == nil and bad2_err, "invalid decode depth option")
local bad3, bad3_err = http.json_encode("x", { max_output = -1 })
check(bad3 == nil and bad3_err, "invalid max_output option")
local limited, limited_err = http.json_encode(string.rep("a", 100), { max_output = 10 })
check(limited == nil and limited_err, "max_output limit")

print("ok: json tests")
