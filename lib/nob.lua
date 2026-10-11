-- nob.lua - Full-featured C-style build system for LuaJIT (FFI-accelerated)

local ffi = require("ffi")
local bit = require("bit")

local nob = {
    is_windows = (ffi.os == "Windows")
}

-------------------------------------------------------------------------------
-- C Declarations (POSIX & Win32)
-------------------------------------------------------------------------------

if nob.is_windows then
    ffi.cdef[[
        typedef struct _FILETIME {
            unsigned long dwLowDateTime;
            unsigned long dwHighDateTime;
        } FILETIME;

        typedef struct _WIN32_FILE_ATTRIBUTE_DATA {
            unsigned long dwFileAttributes;
            FILETIME ftCreationTime;
            FILETIME ftLastAccessTime;
            FILETIME ftLastWriteTime;
            unsigned long nFileSizeHigh;
            unsigned long nFileSizeLow;
        } WIN32_FILE_ATTRIBUTE_DATA;

        typedef struct _STARTUPINFOA {
            unsigned long cb;
            char* lpReserved; char* lpDesktop; char* lpTitle;
            unsigned long dwX, dwY, dwXSize, dwYSize;
            unsigned long dwXCountChars, dwYCountChars, dwFillAttribute;
            unsigned long dwFlags;
            unsigned short wShowWindow, cbReserved2;
            unsigned char* lpReserved2;
            void* hStdInput; void* hStdOutput; void* hStdError;
        } STARTUPINFOA;

        typedef struct _PROCESS_INFORMATION {
            void* hProcess; void* hThread;
            unsigned long dwProcessId, dwThreadId;
        } PROCESS_INFORMATION;

        typedef struct _WIN32_FIND_DATAA {
            unsigned long dwFileAttributes;
            FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
            unsigned long nFileSizeHigh, nFileSizeLow;
            unsigned long dwReserved0, dwReserved1;
            char cFileName[260];
            char cAlternateFileName[14];
        } WIN32_FIND_DATAA;

        int GetFileAttributesExA(const char* lpFileName, int fInfoLevelId, void* lpFileInformation);
        int CreateProcessA(const char* lpAppName, char* lpCmdLine, void* lpProcAttr, void* lpThreadAttr,
                           int bInheritHandles, unsigned long dwCreationFlags, void* lpEnv,
                           const char* lpCurDir, STARTUPINFOA* lpStartupInfo, PROCESS_INFORMATION* lpProcInfo);
        unsigned long WaitForSingleObject(void* hHandle, unsigned long dwMilliseconds);
        unsigned long WaitForMultipleObjects(unsigned long nCount, const void** lpHandles, int bWaitAll, unsigned long dwMilliseconds);
        int GetExitCodeProcess(void* hProcess, unsigned long* lpExitCode);
        int CloseHandle(void* hObject);
        int CreateDirectoryA(const char* lpPathName, void* lpSecurityAttributes);
        void* FindFirstFileA(const char* lpFileName, WIN32_FIND_DATAA* lpFindFileData);
        int FindNextFileA(void* hFindFile, WIN32_FIND_DATAA* lpFindFileData);
        int FindClose(void* hFindFile);
        int CopyFileA(const char* lpExistingFileName, const char* lpNewFileName, int bFailIfExists);
        int DeleteFileA(const char* lpFileName);
    ]]
else
    ffi.cdef[[
        typedef long time_t;

        struct timespec { time_t tv_sec; long tv_nsec; };
        struct stat {
            unsigned long st_dev, st_ino, st_nlink;
            unsigned int st_mode, st_uid, st_gid;
            unsigned long st_rdev; long st_size, st_blksize, st_blocks;
            struct timespec st_atim, st_mtim, st_ctim;
        };

        struct dirent {
            unsigned long d_ino;
            long d_off;
            unsigned short d_reclen;
            unsigned char d_type;
            char d_name[256];
        };

        typedef struct __dirstream DIR;

        int stat(const char *path, struct stat *buf);
        int mkdir(const char *pathname, unsigned int mode);
        int fork(void);
        int execvp(const char *file, char *const argv[]);
        int waitpid(int pid, int *wstatus, int options);
        DIR *opendir(const char *name);
        struct dirent *readdir(DIR *dirp);
        int closedir(DIR *dirp);
        int unlink(const char *pathname);
    ]]
end

-------------------------------------------------------------------------------
-- Path Formatting & File System Operations
-------------------------------------------------------------------------------

function nob.path_join(...)
    local parts = {...}
    local sep = nob.is_windows and "\\" or "/"
    local clean = {}
    for _, p in ipairs(parts) do
        if p and p ~= "" then
            p = p:gsub("[/\\]+", sep)
            table.insert(clean, p)
        end
    end
    return table.concat(clean, sep)
end

function nob.get_mtime(path)
    if nob.is_windows then
        local attr = ffi.new("WIN32_FILE_ATTRIBUTE_DATA")
        if ffi.C.GetFileAttributesExA(path, 0, attr) == 0 then return 0 end
        local ft = attr.ftLastWriteTime
        return tonumber(ft.dwHighDateTime) * 429.4967296 + tonumber(ft.dwLowDateTime) / 1e7
    else
        local st = ffi.new("struct stat")
        if ffi.C.stat(path, st) ~= 0 then return 0 end
        return tonumber(st.st_mtim.tv_sec) + (tonumber(st.st_mtim.tv_nsec) / 1e9)
    end
end

function nob.needs_rebuild(target, sources)
    local target_time = nob.get_mtime(target)
    if target_time == 0 then return true end

    if type(sources) == "string" then sources = { sources } end
    for _, src in ipairs(sources) do
        local src_time = nob.get_mtime(src)
        if src_time == 0 then
            error(string.format("[NOB ERROR] Source '%s' does not exist!", src))
        end
        if src_time > target_time then return true end
    end
    return false
end

function nob.mkdir_if_not_exists(path)
    if nob.get_mtime(path) ~= 0 then return true end
    if nob.is_windows then
        return ffi.C.CreateDirectoryA(path, nil) ~= 0
    else
        return ffi.C.mkdir(path, 511) == 0 -- 0777
    end
end

function nob.rm(path)
    if nob.is_windows then
        return ffi.C.DeleteFileA(path) ~= 0
    else
        return ffi.C.unlink(path) == 0
    end
end

-------------------------------------------------------------------------------
-- Directory Walker
-------------------------------------------------------------------------------

function nob.read_dir(dir_path, recursive)
    local files = {}
    recursive = (recursive == nil) and true or recursive

    if nob.is_windows then
        local search_pattern = nob.path_join(dir_path, "*")
        local find_data = ffi.new("WIN32_FIND_DATAA")
        local handle = ffi.C.FindFirstFileA(search_pattern, find_data)
        if handle == ffi.cast("void*", -1) then return files end

        repeat
            local name = ffi.string(find_data.cFileName)
            if name ~= "." and name ~= ".." then
                local full_path = nob.path_join(dir_path, name)
                local is_dir = (bit.band(find_data.dwFileAttributes, 0x10) ~= 0) -- FILE_ATTRIBUTE_DIRECTORY
                if is_dir then
                    if recursive then
                        local sub = nob.read_dir(full_path, true)
                        for _, f in ipairs(sub) do table.insert(files, f) end
                    end
                else
                    table.insert(files, full_path)
                end
            end
        until ffi.C.FindNextFileA(handle, find_data) == 0
        ffi.C.FindClose(handle)
    else
        local dir = ffi.C.opendir(dir_path)
        if dir == nil then return files end

        while true do
            local entry = ffi.C.readdir(dir)
            if entry == nil then break end
            local name = ffi.string(entry.d_name)
            if name ~= "." and name ~= ".." then
                local full_path = nob.path_join(dir_path, name)
                if entry.d_type == 4 then -- DT_DIR
                    if recursive then
                        local sub = nob.read_dir(full_path, true)
                        for _, f in ipairs(sub) do table.insert(files, f) end
                    end
                else
                    table.insert(files, full_path)
                end
            end
        end
        ffi.C.closedir(dir)
    end
    return files
end

-------------------------------------------------------------------------------
-- Command Builder (Cmd) & Async Parallel Queue (Procs)
-------------------------------------------------------------------------------

local Cmd = {}
Cmd.__index = Cmd

function nob.cmd()
    return setmetatable({ args = {} }, Cmd)
end

function Cmd:append(...)
    for _, arg in ipairs({...}) do
        if type(arg) == "table" then
            for _, v in ipairs(arg) do self:append(v) end
        else
            table.insert(self.args, tostring(arg))
        end
    end
    return self
end

function Cmd:to_string()
    local escaped = {}
    for _, arg in ipairs(self.args) do
        if arg:find(" ") or arg:find("\t") then
            table.insert(escaped, string.format('"%s"', arg))
        else
            table.insert(escaped, arg)
        end
    end
    return table.concat(escaped, " ")
end

-- Native Async Process Spawner
function Cmd:run_async()
    local cmd_str = self:to_string()
    print("[CMD ASYNC] " .. cmd_str)

    if nob.is_windows then
        local si = ffi.new("STARTUPINFOA")
        local pi = ffi.new("PROCESS_INFORMATION")
        si.cb = ffi.sizeof(si)
        local cmd_buf = ffi.new("char[?]", #cmd_str + 1)
        ffi.copy(cmd_buf, cmd_str)

        if ffi.C.CreateProcessA(nil, cmd_buf, nil, nil, 1, 0, nil, nil, si, pi) == 0 then
            error("[NOB ERROR] Failed CreateProcessA")
        end
        ffi.C.CloseHandle(pi.hThread)
        return { handle = pi.hProcess, cmd = cmd_str }
    else
        local pid = ffi.C.fork()
        if pid < 0 then
            error("[NOB ERROR] Fork failed")
        elseif pid == 0 then
            local argv = ffi.new("char*[?]", #self.args + 1)
            for i, arg in ipairs(self.args) do
                argv[i - 1] = ffi.new("char[?]", #arg + 1, arg)
            end
            argv[#self.args] = nil
            ffi.C.execvp(self.args[1], argv)
            os.exit(127)
        else
            return { pid = pid, cmd = cmd_str }
        end
    end
end

function Cmd:run()
    local proc = self:run_async()
    return nob.wait_proc(proc)
end

-------------------------------------------------------------------------------
-- Parallel Job Manager (Procs / -j Support)
-------------------------------------------------------------------------------

local Procs = {}
Procs.__index = Procs

function nob.procs(max_jobs)
    return setmetatable({
        queue = {},
        active = {},
        max_jobs = max_jobs or 4
    }, Procs)
end

function Procs:append(cmd)
    table.insert(self.queue, cmd)
end

function nob.wait_proc(proc)
    if nob.is_windows then
        ffi.C.WaitForSingleObject(proc.handle, 0xFFFFFFFF)
        local exit_code = ffi.new("unsigned long[1]")
        ffi.C.GetExitCodeProcess(proc.handle, exit_code)
        ffi.C.CloseHandle(proc.handle)
        if exit_code[0] ~= 0 then
            error(string.format("[NOB ERROR] Cmd failed (%d): %s", exit_code[0], proc.cmd))
        end
    else
        local wstatus = ffi.new("int[1]")
        ffi.C.waitpid(proc.pid, wstatus, 0)
        local status = wstatus[0]
        local exit_code = bit.rshift(bit.band(status, 0xff00), 8)
        if exit_code ~= 0 then
            error(string.format("[NOB ERROR] Cmd failed (%d): %s", exit_code[0], proc.cmd))
        end
    end
    return true
end

function Procs:wait_all()
    while #self.queue > 0 or #self.active > 0 do
        -- Fill active slots
        while #self.active < self.max_jobs and #self.queue > 0 do
            local cmd = table.remove(self.queue, 1)
            local proc = cmd:run_async()
            table.insert(self.active, proc)
        end

        -- Poll/Wait for active processes
        if #self.active > 0 then
            local proc = table.remove(self.active, 1)
            nob.wait_proc(proc)
        end
    end
end

-------------------------------------------------------------------------------
-- Auto Header Dependency Scanner (gcc / clang -M)
-------------------------------------------------------------------------------

function nob.get_c_dependencies(cc, source, cflags)
    local cmd = nob.cmd():append(cc, "-M", source, cflags)
    local handle = io.popen(cmd:to_string())
    if not handle then return { source } end

    local result = handle:read("*a")
    handle:close()

    local deps = {}
    -- Remove target prefix (e.g. "main.o: ") and line continuation backslashes
    result = result:gsub("^.-:%s*", ""):gsub("\\\n", " ")
    for dep in result:gmatch("%S+") do
        table.insert(deps, dep)
    end
    return deps
end

return nob
