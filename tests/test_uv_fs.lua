local uv = require("uv")

local function check(cond, msg)
    if not cond then error(msg or "check failed", 2) end
end

local root = uv.cwd() .. "/tests/.uvfs-test-" .. tostring(os.time())
local file = root .. "/data.txt"
local copied = root .. "/copy.txt"
local renamed = root .. "/renamed.txt"

local ok, err = uv.fs_mkdir(root, 448)
check(ok, err)

ok, err = uv.fs_stat(root)
check(ok and ok.type == "directory", "mkdir stat failed: " .. tostring(err))

local f, open_err = uv.fs_open(file, "w+", 420)
check(f, open_err)
check(f:fd() >= 0, "file fd invalid")
check(f:write("hello world") == 11, "sync write failed")
local st = f:stat()
check(st and st.type == "file" and st.size == 11, "fstat failed")
check(f:close(), "sync close failed")
check(f:is_closing(), "closed file not marked closing")

local r, read_err = uv.fs_open(file, "r", 0)
check(r, read_err)
check(r:read(5, 0) == "hello", "offset read failed")
check(r:read(5) == "hello", "sequential read failed")
check(r:read(5, 6) == "world", "second offset read failed")
check(r:close(), "read close failed")

local async_read = nil
local ar, ar_err = uv.fs_open(file, "r", 0)
check(ar, ar_err)
ok, err = ar:read(5, 0, function(cb_err, data)
    async_read = { cb_err, data }
    ar:close()
end)
check(ok, err)
uv.run()
check(async_read and async_read[1] == nil and async_read[2] == "hello",
      "async read failed")

local async_stat = nil
ok, err = uv.fs_stat(file, function(cb_err, data)
    async_stat = { cb_err, data }
end)
check(ok, err)
uv.run()
check(async_stat and async_stat[1] == nil and async_stat[2].size == 11,
      "async stat failed")

local async_scandir = nil
ok, err = uv.fs_scandir(root, function(cb_err, data)
    async_scandir = { cb_err, data }
end)
check(ok, err)
uv.run()
check(async_scandir and async_scandir[1] == nil, "async scandir failed")
local saw_file = false
for _, ent in ipairs(async_scandir[2]) do
    if ent.name == "data.txt" and ent.type == "file" then saw_file = true end
end
check(saw_file, "async scandir missing file")

local async_missing = nil
ok, err = uv.fs_stat(root .. "/missing", function(cb_err, data)
    async_missing = { cb_err, data }
end)
check(ok, err)
uv.run()
check(async_missing and async_missing[1] and async_missing[2] == nil,
      "async missing file should return an error")

ok, err = uv.fs_copyfile(file, copied)
check(ok, err)
local cst = uv.fs_stat(copied)
check(cst and cst.size == 11, "copyfile failed")
ok, err = uv.fs_rename(copied, renamed)
check(ok, err)
ok, err = uv.fs_chmod(renamed, 384)
check(ok, err)
ok, err = uv.fs_utime(renamed, 1000000000, 1000000000)
check(ok, err)
local ust = uv.fs_stat(renamed)
check(ust and math.floor(ust.mtime_sec) == 1000000000, "utime failed")

local real = uv.fs_realpath(renamed)
check(real and real:find("renamed.txt", 1, true), "realpath failed")
local lst = uv.fs_lstat(renamed)
check(lst and lst.type == "file", "lstat failed")

local nil_path, missing_err = uv.fs_stat(root .. "/missing")
check(nil_path == nil and type(missing_err) == "string", "missing stat error")

local bad_ok, bad_err = pcall(function() uv.fs_open(file, "bogus", 0) end)
check(not bad_ok and tostring(bad_err):find("invalid flags", 1, true),
      "invalid open flags should fail")

ok, err = uv.fs_unlink(file)
check(ok, err)
ok, err = uv.fs_unlink(renamed)
check(ok, err)
ok, err = uv.fs_rmdir(root)
check(ok, err)

local exe = uv.exepath()
check(type(exe) == "string" and #exe > 0, "exepath failed")
local cwd = uv.cwd()
check(type(cwd) == "string" and #cwd > 0, "cwd failed")

print("ok: uv fs tests")
