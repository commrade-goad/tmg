#include "uv_module.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fcntl.h>
#include <sys/stat.h>

#include <uv.h>

#include <lauxlib.h>
#include <lua.h>

#define UV_FILE_MT "tmg.uv.file"

typedef struct uv_file_ud uv_file_ud_t;
typedef struct uv_fs_ctx uv_fs_ctx_t;

struct uv_file_ud {
    uv_file fd;
    lua_State *L;
    uv_file_ud_t *next;
    int self_ref;
    int close_ref;
    int pending_ops;
    bool linked;
    bool closing;
    bool closed;
    bool close_inflight;
};

typedef enum {
    UFS_STAT,
    UFS_LSTAT,
    UFS_FSTAT,
    UFS_OPEN,
    UFS_READ,
    UFS_WRITE,
    UFS_CLOSE,
    UFS_UNLINK,
    UFS_MKDIR,
    UFS_RMDIR,
    UFS_RENAME,
    UFS_COPYFILE,
    UFS_CHMOD,
    UFS_CHOWN,
    UFS_UTIME,
    UFS_REALPATH,
    UFS_SCANDIR
} uv_fs_op_t;

struct uv_fs_ctx {
    uv_fs_t req;
    uv_fs_op_t op;
    lua_State *L;
    uv_file_ud_t *file;
    uv_fs_ctx_t *next;
    int cb_ref;
    char *path1;
    char *path2;
    char *buf;
    size_t buf_len;
    bool shutdown;
};

static uv_file_ud_t *g_files;
static uv_fs_ctx_t *g_fs_ctxs;

static void clear_ref(lua_State *L, int *slot) {
    if (*slot != LUA_NOREF && *slot != LUA_REFNIL) {
        luaL_unref(L, LUA_REGISTRYINDEX, *slot);
    }
    *slot = LUA_NOREF;
}

static void replace_ref(lua_State *L, int *slot, int idx) {
    clear_ref(L, slot);
    if (idx > 0 && !lua_isnil(L, idx)) {
        lua_pushvalue(L, idx);
        *slot = luaL_ref(L, LUA_REGISTRYINDEX);
    }
}

static char *dup_string(const char *s) {
    if (!s) s = "";
    size_t len = strlen(s);
    char *p = malloc(len + 1);
    if (!p) return NULL;
    memcpy(p, s, len + 1);
    return p;
}

/* ---- file userdata ----------------------------------------------------- */

static void file_unlink(uv_file_ud_t *f) {
    if (!f || !f->linked) return;
    uv_file_ud_t **pp = &g_files;
    while (*pp) {
        if (*pp == f) {
            *pp = f->next;
            break;
        }
        pp = &(*pp)->next;
    }
    f->linked = false;
}

static void file_link(uv_file_ud_t *f) {
    f->next = g_files;
    g_files = f;
    f->linked = true;
}

static uv_file_ud_t *push_file(lua_State *L, uv_file fd) {
    uv_file_ud_t *f = (uv_file_ud_t *)lua_newuserdata(L, sizeof(*f));
    memset(f, 0, sizeof(*f));
    f->fd = fd;
    f->L = L;
    f->self_ref = LUA_NOREF;
    f->close_ref = LUA_NOREF;
    luaL_getmetatable(L, UV_FILE_MT);
    lua_setmetatable(L, -2);
    file_link(f);
    return f;
}

static uv_file_ud_t *check_file(lua_State *L) {
    uv_file_ud_t *f = (uv_file_ud_t *)luaL_checkudata(L, 1, UV_FILE_MT);
    if (!f || f->closed)
        luaL_error(L, "uv: file is closed");
    return f;
}

static uv_file_ud_t *check_file_open(lua_State *L) {
    uv_file_ud_t *f = check_file(L);
    if (f->closing)
        luaL_error(L, "uv: file is closing");
    return f;
}

static void file_pending_hold(uv_file_ud_t *f) {
    f->pending_ops++;
}

static void file_pending_add(lua_State *L, uv_file_ud_t *f, int self_idx) {
    if (f->pending_ops++ == 0 && self_idx > 0) {
        lua_pushvalue(L, self_idx);
        f->self_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    }
}

static void file_start_close_async(uv_file_ud_t *f, int self_idx);

static void file_pending_del(uv_file_ud_t *f) {
    if (!f) return;
    if (f->pending_ops > 0)
        f->pending_ops--;

    if (f->pending_ops == 0) {
        if (f->closing && !f->closed && !f->close_inflight) {
            /* The existing self reference (if any) keeps the userdata alive
             * while the deferred close runs. */
            file_start_close_async(f, 0);
            return;
        }
        clear_ref(f->L, &f->self_ref);
    }
}

static bool file_close_sync(uv_file_ud_t *f) {
    if (!f || f->closed) return true;
    uv_fs_t req;
    int rc = uv_fs_close(NULL, &req, f->fd, NULL);
    uv_fs_req_cleanup(&req);
    if (rc < 0) return false;
    f->closed = true;
    clear_ref(f->L, &f->close_ref);
    return true;
}

static void fs_ctx_remove(uv_fs_ctx_t *ctx) {
    uv_fs_ctx_t **pp = &g_fs_ctxs;
    while (*pp) {
        if (*pp == ctx) {
            *pp = ctx->next;
            return;
        }
        pp = &(*pp)->next;
    }
}

static void fs_ctx_destroy(uv_fs_ctx_t *ctx) {
    if (!ctx) return;
    fs_ctx_remove(ctx);
    if (ctx->cb_ref != LUA_NOREF)
        clear_ref(ctx->L, &ctx->cb_ref);
    uv_fs_req_cleanup(&ctx->req);
    free(ctx->path1);
    free(ctx->path2);
    free(ctx->buf);
    free(ctx);
}

/* ---- stat conversion --------------------------------------------------- */

static void push_u64(lua_State *L, uint64_t v) {
    if (v <= (uint64_t)PTRDIFF_MAX)
        lua_pushinteger(L, (lua_Integer)v);
    else
        lua_pushnumber(L, (lua_Number)v);
}

static void push_time(lua_State *L, const char *name, const uv_timespec_t *ts) {
    lua_pushnumber(L, (lua_Number)ts->tv_sec +
                      (lua_Number)ts->tv_nsec / 1000000000.0);
    lua_setfield(L, -2, name);

    char key[32];
    snprintf(key, sizeof(key), "%s_sec", name);
    push_u64(L, (uint64_t)ts->tv_sec);
    lua_setfield(L, -2, key);

    snprintf(key, sizeof(key), "%s_nsec", name);
    push_u64(L, (uint64_t)ts->tv_nsec);
    lua_setfield(L, -2, key);
}

static const char *stat_type(uint64_t mode) {
    if (S_ISREG((mode_t)mode)) return "file";
    if (S_ISDIR((mode_t)mode)) return "directory";
    if (S_ISLNK((mode_t)mode)) return "symlink";
    if (S_ISFIFO((mode_t)mode)) return "fifo";
    if (S_ISSOCK((mode_t)mode)) return "socket";
    if (S_ISCHR((mode_t)mode)) return "char";
    if (S_ISBLK((mode_t)mode)) return "block";
    return "unknown";
}

static void push_stat(lua_State *L, const uv_stat_t *s) {
    lua_createtable(L, 0, 28);
    push_u64(L, s->st_dev); lua_setfield(L, -2, "dev");
    push_u64(L, s->st_mode); lua_setfield(L, -2, "mode");
    push_u64(L, s->st_nlink); lua_setfield(L, -2, "nlink");
    push_u64(L, s->st_uid); lua_setfield(L, -2, "uid");
    push_u64(L, s->st_gid); lua_setfield(L, -2, "gid");
    push_u64(L, s->st_rdev); lua_setfield(L, -2, "rdev");
    push_u64(L, s->st_ino); lua_setfield(L, -2, "ino");
    push_u64(L, s->st_size); lua_setfield(L, -2, "size");
    push_u64(L, s->st_blksize); lua_setfield(L, -2, "blksize");
    push_u64(L, s->st_blocks); lua_setfield(L, -2, "blocks");
    push_u64(L, s->st_flags); lua_setfield(L, -2, "flags");
    push_u64(L, s->st_gen); lua_setfield(L, -2, "gen");

    push_time(L, "atime", &s->st_atim);
    push_time(L, "mtime", &s->st_mtim);
    push_time(L, "ctime", &s->st_ctim);
    push_time(L, "birthtime", &s->st_birthtim);

    lua_pushstring(L, stat_type(s->st_mode));
    lua_setfield(L, -2, "type");
}

/* ---- async completion -------------------------------------------------- */

static void fs_ctx_complete(uv_fs_ctx_t *ctx, int rc);

static void fs_cb(uv_fs_t *req) {
    uv_fs_ctx_t *ctx = (uv_fs_ctx_t *)req->data;
    if (!ctx) return;
    fs_ctx_complete(ctx, (int)req->result);
}

static void fs_ctx_complete(uv_fs_ctx_t *ctx, int rc) {
    lua_State *L = ctx->L;
    uv_file_ud_t *f = ctx->file;
    bool shutdown = ctx->shutdown;
    int cb_ref = ctx->cb_ref;
    ctx->cb_ref = LUA_NOREF;

    fs_ctx_remove(ctx);

    if (ctx->op == UFS_CLOSE && f && rc >= 0) {
        f->closed = true;
        f->close_inflight = false;
    }
    if (ctx->op == UFS_OPEN && shutdown && rc >= 0) {
        uv_fs_t close_req;
        uv_fs_close(NULL, &close_req, (uv_file)rc, NULL);
        uv_fs_req_cleanup(&close_req);
    }

    if (!shutdown && cb_ref != LUA_NOREF) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, cb_ref);
        int nargs = 2;

        if (rc < 0) {
            lua_pushstring(L, uv_strerror(rc));
            lua_pushnil(L);
        } else {
            lua_pushnil(L);
            switch (ctx->op) {
            case UFS_STAT:
            case UFS_LSTAT:
            case UFS_FSTAT:
                push_stat(L, &ctx->req.statbuf);
                break;
            case UFS_OPEN:
                push_file(L, (uv_file)rc);
                break;
            case UFS_READ:
                lua_pushlstring(L, ctx->buf ? ctx->buf : "", (size_t)rc);
                break;
            case UFS_WRITE:
                lua_pushinteger(L, (lua_Integer)rc);
                break;
            case UFS_REALPATH:
                lua_pushstring(L, ctx->req.ptr ? (const char *)ctx->req.ptr : "");
                break;
            case UFS_SCANDIR: {
                lua_newtable(L);
                uv_fs_t *sr = &ctx->req;
                uv_dirent_t dent;
                int n = 0;
                while (uv_fs_scandir_next(sr, &dent) != UV_EOF) {
                    lua_createtable(L, 0, 2);
                    lua_pushstring(L, dent.name);
                    lua_setfield(L, -2, "name");
                    const char *type = "unknown";
                    switch (dent.type) {
                    case UV_DIRENT_FILE: type = "file"; break;
                    case UV_DIRENT_DIR: type = "directory"; break;
                    case UV_DIRENT_LINK: type = "link"; break;
                    case UV_DIRENT_FIFO: type = "fifo"; break;
                    case UV_DIRENT_SOCKET: type = "socket"; break;
                    case UV_DIRENT_CHAR: type = "char"; break;
                    case UV_DIRENT_BLOCK: type = "block"; break;
                    default: break;
                    }
                    lua_pushstring(L, type);
                    lua_setfield(L, -2, "type");
                    lua_rawseti(L, -2, ++n);
                }
                break;
            }
            default:
                lua_pushboolean(L, 1);
                break;
            }
        }

        if (lua_pcall(L, nargs, 0, 0) != LUA_OK) {
            const char *msg = lua_tostring(L, -1);
            fprintf(stderr, "uv: fs callback error: %s\n",
                    msg ? msg : "unknown error");
            lua_pop(L, 1);
        }
    }

    clear_ref(L, &cb_ref);
    if (f) {
        if (ctx->op == UFS_CLOSE)
            f->close_inflight = false;
        file_pending_del(f);
    }
    fs_ctx_destroy(ctx);
}

static uv_fs_ctx_t *fs_ctx_new(lua_State *L, uv_fs_op_t op, int cb_idx) {
    uv_fs_ctx_t *ctx = (uv_fs_ctx_t *)calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;
    ctx->op = op;
    ctx->L = L;
    ctx->cb_ref = LUA_NOREF;
    if (cb_idx > 0)
        replace_ref(L, &ctx->cb_ref, cb_idx);
    ctx->next = g_fs_ctxs;
    g_fs_ctxs = ctx;
    ctx->req.data = ctx;
    return ctx;
}

static int fs_fail(lua_State *L, int rc) {
    lua_pushnil(L);
    lua_pushstring(L, uv_strerror(rc));
    return 2;
}

static bool fs_has_cb(lua_State *L, int idx) {
    if (lua_isnoneornil(L, idx)) return false;
    luaL_checktype(L, idx, LUA_TFUNCTION);
    return true;
}

static int fs_submit_result(lua_State *L, uv_fs_ctx_t *ctx, int rc) {
    if (rc < 0) {
        /* libuv did not schedule the request.  Complete synchronously. */
        fs_ctx_complete(ctx, rc);
        lua_pushnil(L);
        lua_pushstring(L, uv_strerror(rc));
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

/* ---- flags and arguments ---------------------------------------------- */

static int parse_flags(const char *s) {
    if (!s) return -1;
    if (strcmp(s, "r") == 0 || strcmp(s, "rs") == 0 || strcmp(s, "sr") == 0)
        return O_RDONLY | (s[0] == 'r' && s[1] == 's' ? O_SYNC : 0);
    if (strcmp(s, "r+") == 0)
        return O_RDWR;
    if (strcmp(s, "rs+") == 0 || strcmp(s, "sr+") == 0)
        return O_RDWR | O_SYNC;
    if (strcmp(s, "w") == 0)
        return O_WRONLY | O_CREAT | O_TRUNC;
    if (strcmp(s, "wx") == 0)
        return O_WRONLY | O_CREAT | O_TRUNC | O_EXCL;
    if (strcmp(s, "w+") == 0)
        return O_RDWR | O_CREAT | O_TRUNC;
    if (strcmp(s, "wx+") == 0)
        return O_RDWR | O_CREAT | O_TRUNC | O_EXCL;
    if (strcmp(s, "a") == 0)
        return O_WRONLY | O_CREAT | O_APPEND;
    if (strcmp(s, "ax") == 0)
        return O_WRONLY | O_CREAT | O_APPEND | O_EXCL;
    if (strcmp(s, "a+") == 0)
        return O_RDWR | O_CREAT | O_APPEND;
    if (strcmp(s, "ax+") == 0)
        return O_RDWR | O_CREAT | O_APPEND | O_EXCL;
    return -1;
}

static bool check_offset(lua_State *L, int idx, int64_t *out) {
    lua_Integer v = luaL_checkinteger(L, idx);
    *out = (int64_t)v;
    return true;
}

/* ---- synchronous operations ------------------------------------------- */

static int fs_stat_sync(lua_State *L, const char *path, bool lstat) {
    uv_fs_t req;
    int rc = lstat ? uv_fs_lstat(NULL, &req, path, NULL)
                   : uv_fs_stat(NULL, &req, path, NULL);
    if (rc < 0) {
        uv_fs_req_cleanup(&req);
        return fs_fail(L, rc);
    }
    push_stat(L, &req.statbuf);
    uv_fs_req_cleanup(&req);
    return 1;
}

static int fs_open_sync(lua_State *L, const char *path, int flags, int mode) {
    uv_fs_t req;
    int rc = uv_fs_open(NULL, &req, path, flags, mode, NULL);
    if (rc < 0) {
        uv_fs_req_cleanup(&req);
        return fs_fail(L, rc);
    }
    push_file(L, (uv_file)rc);
    uv_fs_req_cleanup(&req);
    return 1;
}

static int fs_read_sync(lua_State *L, uv_file_ud_t *f, size_t size,
                        int64_t offset) {
    char *buf = malloc(size ? size : 1);
    if (!buf) return luaL_error(L, "uv: out of memory");
    uv_buf_t b = uv_buf_init(buf, (unsigned int)size);
    uv_fs_t req;
    int rc = uv_fs_read(NULL, &req, f->fd, &b, 1, offset, NULL);
    uv_fs_req_cleanup(&req);
    if (rc < 0) {
        free(buf);
        return fs_fail(L, rc);
    }
    lua_pushlstring(L, buf, (size_t)rc);
    free(buf);
    return 1;
}

static int fs_write_sync(lua_State *L, uv_file_ud_t *f, const char *data,
                         size_t len, int64_t offset) {
    uv_buf_t b = uv_buf_init((char *)data, (unsigned int)len);
    uv_fs_t req;
    int rc = uv_fs_write(NULL, &req, f->fd, &b, 1, offset, NULL);
    uv_fs_req_cleanup(&req);
    if (rc < 0) return fs_fail(L, rc);
    lua_pushinteger(L, (lua_Integer)rc);
    return 1;
}

/* ---- async operations -------------------------------------------------- */

static void file_start_close_async(uv_file_ud_t *f, int self_idx) {
    if (!f || f->closed || f->close_inflight) return;
    int cb = f->close_ref;
    f->close_ref = LUA_NOREF;

    uv_fs_ctx_t *ctx = fs_ctx_new(f->L, UFS_CLOSE, 0);
    if (!ctx) {
        clear_ref(f->L, &cb);
        if (self_idx > 0) luaL_error(f->L, "uv: out of memory");
        return;
    }
    ctx->file = f;
    ctx->cb_ref = cb;
    f->close_inflight = true;
    if (self_idx > 0) {
        lua_pushvalue(f->L, self_idx);
        f->self_ref = luaL_ref(f->L, LUA_REGISTRYINDEX);
        f->pending_ops++;
    } else {
        file_pending_hold(f);
    }

    int rc = uv_fs_close(uv_default_loop(), &ctx->req, f->fd, fs_cb);
    if (rc < 0) {
        f->close_inflight = false;
        if (cb != LUA_NOREF) {
            ctx->cb_ref = cb;
            fs_ctx_complete(ctx, rc);
        } else {
            fs_ctx_destroy(ctx);
            file_pending_del(f);
        }
    }
}

/* ---- module functions -------------------------------------------------- */

static int l_fs_stat(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    bool lstat = false;
    if (!lua_isnoneornil(L, 2))
        luaL_checktype(L, 2, LUA_TFUNCTION);
    if (lua_isnoneornil(L, 2))
        return fs_stat_sync(L, path, lstat);

    uv_fs_ctx_t *ctx = fs_ctx_new(L, UFS_STAT, 2);
    if (!ctx) return luaL_error(L, "uv.fs_stat: out of memory");
    ctx->path1 = dup_string(path);
    if (!ctx->path1) return luaL_error(L, "uv.fs_stat: out of memory");
    int rc = uv_fs_stat(uv_default_loop(), &ctx->req, ctx->path1, fs_cb);
    return fs_submit_result(L, ctx, rc);
}

static int l_fs_lstat(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    if (!lua_isnoneornil(L, 2))
        luaL_checktype(L, 2, LUA_TFUNCTION);
    if (lua_isnoneornil(L, 2))
        return fs_stat_sync(L, path, true);

    uv_fs_ctx_t *ctx = fs_ctx_new(L, UFS_LSTAT, 2);
    if (!ctx) return luaL_error(L, "uv.fs_lstat: out of memory");
    ctx->path1 = dup_string(path);
    if (!ctx->path1) return luaL_error(L, "uv.fs_lstat: out of memory");
    int rc = uv_fs_lstat(uv_default_loop(), &ctx->req, ctx->path1, fs_cb);
    return fs_submit_result(L, ctx, rc);
}

static int l_fs_open(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    const char *flagstr = luaL_checkstring(L, 2);
    int flags = parse_flags(flagstr);
    if (flags < 0) return luaL_error(L, "uv.fs_open: invalid flags '%s'", flagstr);
    lua_Integer mode = luaL_optinteger(L, 3, 0644);
    if (mode < 0 || mode > 07777)
        return luaL_error(L, "uv.fs_open: invalid mode");
    if (!lua_isnoneornil(L, 4))
        luaL_checktype(L, 4, LUA_TFUNCTION);
    if (lua_isnoneornil(L, 4))
        return fs_open_sync(L, path, flags, (int)mode);

    uv_fs_ctx_t *ctx = fs_ctx_new(L, UFS_OPEN, 4);
    if (!ctx) return luaL_error(L, "uv.fs_open: out of memory");
    ctx->path1 = dup_string(path);
    if (!ctx->path1) return luaL_error(L, "uv.fs_open: out of memory");
    int rc = uv_fs_open(uv_default_loop(), &ctx->req, ctx->path1, flags,
                        (int)mode, fs_cb);
    return fs_submit_result(L, ctx, rc);
}

static void fs_start_path_async(lua_State *L, uv_fs_op_t op, int cb_idx,
                                const char *path) {
    uv_fs_ctx_t *ctx = fs_ctx_new(L, op, cb_idx);
    if (!ctx) luaL_error(L, "uv: out of memory");
    ctx->path1 = dup_string(path);
    if (!ctx->path1) luaL_error(L, "uv: out of memory");
    int rc = 0;
    switch (op) {
    case UFS_UNLINK:
        rc = uv_fs_unlink(uv_default_loop(), &ctx->req, ctx->path1, fs_cb);
        break;
    case UFS_RMDIR:
        rc = uv_fs_rmdir(uv_default_loop(), &ctx->req, ctx->path1, fs_cb);
        break;
    case UFS_REALPATH:
        rc = uv_fs_realpath(uv_default_loop(), &ctx->req, ctx->path1, fs_cb);
        break;
    case UFS_SCANDIR:
        rc = uv_fs_scandir(uv_default_loop(), &ctx->req, ctx->path1, 0, fs_cb);
        break;
    default:
        break;
    }
    if (rc < 0) {
        fs_ctx_complete(ctx, rc);
        lua_pushnil(L);
        lua_pushstring(L, uv_strerror(rc));
    } else {
        lua_pushboolean(L, 1);
    }
}

static int l_fs_unlink(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    if (!fs_has_cb(L, 2)) {
        uv_fs_t req;
        int rc = uv_fs_unlink(NULL, &req, path, NULL);
        uv_fs_req_cleanup(&req);
        if (rc < 0) return fs_fail(L, rc);
        lua_pushboolean(L, 1);
        return 1;
    }
    fs_start_path_async(L, UFS_UNLINK, 2, path);
    return 1;
}

static int l_fs_rmdir(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    if (!fs_has_cb(L, 2)) {
        uv_fs_t req;
        int rc = uv_fs_rmdir(NULL, &req, path, NULL);
        uv_fs_req_cleanup(&req);
        if (rc < 0) return fs_fail(L, rc);
        lua_pushboolean(L, 1);
        return 1;
    }
    fs_start_path_async(L, UFS_RMDIR, 2, path);
    return 1;
}

static int l_fs_mkdir(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    lua_Integer mode = luaL_optinteger(L, 2, 0755);
    if (mode < 0 || mode > 07777)
        return luaL_error(L, "uv.fs_mkdir: invalid mode");
    int cb_idx = 3;
    if (!lua_isnoneornil(L, 3))
        luaL_checktype(L, 3, LUA_TFUNCTION);
    if (lua_isnoneornil(L, 3)) {
        uv_fs_t req;
        int rc = uv_fs_mkdir(NULL, &req, path, (int)mode, NULL);
        uv_fs_req_cleanup(&req);
        if (rc < 0) return fs_fail(L, rc);
        lua_pushboolean(L, 1);
        return 1;
    }
    uv_fs_ctx_t *ctx = fs_ctx_new(L, UFS_MKDIR, cb_idx);
    if (!ctx) return luaL_error(L, "uv.fs_mkdir: out of memory");
    ctx->path1 = dup_string(path);
    if (!ctx->path1) return luaL_error(L, "uv.fs_mkdir: out of memory");
    int rc = uv_fs_mkdir(uv_default_loop(), &ctx->req, ctx->path1,
                         (int)mode, fs_cb);
    return fs_submit_result(L, ctx, rc);
}

static int l_fs_rename(lua_State *L) {
    const char *oldp = luaL_checkstring(L, 1);
    const char *newp = luaL_checkstring(L, 2);
    if (!lua_isnoneornil(L, 3))
        luaL_checktype(L, 3, LUA_TFUNCTION);
    if (lua_isnoneornil(L, 3)) {
        uv_fs_t req;
        int rc = uv_fs_rename(NULL, &req, oldp, newp, NULL);
        uv_fs_req_cleanup(&req);
        if (rc < 0) return fs_fail(L, rc);
        lua_pushboolean(L, 1);
        return 1;
    }
    uv_fs_ctx_t *ctx = fs_ctx_new(L, UFS_RENAME, 3);
    if (!ctx) return luaL_error(L, "uv.fs_rename: out of memory");
    ctx->path1 = dup_string(oldp);
    ctx->path2 = dup_string(newp);
    if (!ctx->path1 || !ctx->path2) return luaL_error(L, "uv.fs_rename: out of memory");
    int rc = uv_fs_rename(uv_default_loop(), &ctx->req, ctx->path1, ctx->path2, fs_cb);
    return fs_submit_result(L, ctx, rc);
}

static int l_fs_copyfile(lua_State *L) {
    const char *src = luaL_checkstring(L, 1);
    const char *dst = luaL_checkstring(L, 2);
    lua_Integer flags = 0;
    int cb_idx = 0;
    if (lua_isfunction(L, 3)) {
        cb_idx = 3;
    } else if (!lua_isnoneornil(L, 3)) {
        flags = luaL_checkinteger(L, 3);
        if (!lua_isnoneornil(L, 4)) {
            luaL_checktype(L, 4, LUA_TFUNCTION);
            cb_idx = 4;
        }
    }
    if (!cb_idx) {
        uv_fs_t req;
        int rc = uv_fs_copyfile(NULL, &req, src, dst, (int)flags, NULL);
        uv_fs_req_cleanup(&req);
        if (rc < 0) return fs_fail(L, rc);
        lua_pushboolean(L, 1);
        return 1;
    }
    uv_fs_ctx_t *ctx = fs_ctx_new(L, UFS_COPYFILE, cb_idx);
    if (!ctx) return luaL_error(L, "uv.fs_copyfile: out of memory");
    ctx->path1 = dup_string(src);
    ctx->path2 = dup_string(dst);
    if (!ctx->path1 || !ctx->path2) return luaL_error(L, "uv.fs_copyfile: out of memory");
    int rc = uv_fs_copyfile(uv_default_loop(), &ctx->req, ctx->path1,
                            ctx->path2, (int)flags, fs_cb);
    return fs_submit_result(L, ctx, rc);
}

static int l_fs_chmod(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    lua_Integer mode = luaL_checkinteger(L, 2);
    if (mode < 0 || mode > 07777)
        return luaL_error(L, "uv.fs_chmod: invalid mode");
    if (!lua_isnoneornil(L, 3))
        luaL_checktype(L, 3, LUA_TFUNCTION);
    if (lua_isnoneornil(L, 3)) {
        uv_fs_t req;
        int rc = uv_fs_chmod(NULL, &req, path, (int)mode, NULL);
        uv_fs_req_cleanup(&req);
        if (rc < 0) return fs_fail(L, rc);
        lua_pushboolean(L, 1);
        return 1;
    }
    uv_fs_ctx_t *ctx = fs_ctx_new(L, UFS_CHMOD, 3);
    if (!ctx) return luaL_error(L, "uv.fs_chmod: out of memory");
    ctx->path1 = dup_string(path);
    if (!ctx->path1) return luaL_error(L, "uv.fs_chmod: out of memory");
    int rc = uv_fs_chmod(uv_default_loop(), &ctx->req, ctx->path1,
                         (int)mode, fs_cb);
    return fs_submit_result(L, ctx, rc);
}

static int l_fs_chown(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    lua_Integer uid = luaL_checkinteger(L, 2);
    lua_Integer gid = luaL_checkinteger(L, 3);
    if (uid < 0 || gid < 0)
        return luaL_error(L, "uv.fs_chown: invalid uid/gid");
    if (!lua_isnoneornil(L, 4))
        luaL_checktype(L, 4, LUA_TFUNCTION);
    if (lua_isnoneornil(L, 4)) {
        uv_fs_t req;
        int rc = uv_fs_chown(NULL, &req, path, (uv_uid_t)uid,
                             (uv_gid_t)gid, NULL);
        uv_fs_req_cleanup(&req);
        if (rc < 0) return fs_fail(L, rc);
        lua_pushboolean(L, 1);
        return 1;
    }
    uv_fs_ctx_t *ctx = fs_ctx_new(L, UFS_CHOWN, 4);
    if (!ctx) return luaL_error(L, "uv.fs_chown: out of memory");
    ctx->path1 = dup_string(path);
    if (!ctx->path1) return luaL_error(L, "uv.fs_chown: out of memory");
    int rc = uv_fs_chown(uv_default_loop(), &ctx->req, ctx->path1,
                         (uv_uid_t)uid, (uv_gid_t)gid, fs_cb);
    return fs_submit_result(L, ctx, rc);
}

static int l_fs_utime(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    lua_Number atime = luaL_checknumber(L, 2);
    lua_Number mtime = luaL_checknumber(L, 3);
    if (!lua_isnoneornil(L, 4))
        luaL_checktype(L, 4, LUA_TFUNCTION);
    if (lua_isnoneornil(L, 4)) {
        uv_fs_t req;
        int rc = uv_fs_utime(NULL, &req, path, atime, mtime, NULL);
        uv_fs_req_cleanup(&req);
        if (rc < 0) return fs_fail(L, rc);
        lua_pushboolean(L, 1);
        return 1;
    }
    uv_fs_ctx_t *ctx = fs_ctx_new(L, UFS_UTIME, 4);
    if (!ctx) return luaL_error(L, "uv.fs_utime: out of memory");
    ctx->path1 = dup_string(path);
    if (!ctx->path1) return luaL_error(L, "uv.fs_utime: out of memory");
    int rc = uv_fs_utime(uv_default_loop(), &ctx->req, ctx->path1,
                         atime, mtime, fs_cb);
    return fs_submit_result(L, ctx, rc);
}

static int l_fs_realpath(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    if (!lua_isnoneornil(L, 2))
        luaL_checktype(L, 2, LUA_TFUNCTION);
    if (lua_isnoneornil(L, 2)) {
        uv_fs_t req;
        int rc = uv_fs_realpath(NULL, &req, path, NULL);
        if (rc < 0) {
            uv_fs_req_cleanup(&req);
            return fs_fail(L, rc);
        }
        lua_pushstring(L, req.ptr ? (const char *)req.ptr : "");
        uv_fs_req_cleanup(&req);
        return 1;
    }
    fs_start_path_async(L, UFS_REALPATH, 2, path);
    return 1;
}

static int l_fs_scandir(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    if (!lua_isnoneornil(L, 2))
        luaL_checktype(L, 2, LUA_TFUNCTION);
    if (lua_isnoneornil(L, 2)) {
        uv_fs_t req;
        int rc = uv_fs_scandir(NULL, &req, path, 0, NULL);
        if (rc < 0) {
            uv_fs_req_cleanup(&req);
            return fs_fail(L, rc);
        }
        lua_newtable(L);
        uv_dirent_t dent;
        int n = 0;
        while (uv_fs_scandir_next(&req, &dent) != UV_EOF) {
            lua_createtable(L, 0, 2);
            lua_pushstring(L, dent.name);
            lua_setfield(L, -2, "name");
            const char *type = "unknown";
            switch (dent.type) {
            case UV_DIRENT_FILE: type = "file"; break;
            case UV_DIRENT_DIR: type = "directory"; break;
            case UV_DIRENT_LINK: type = "link"; break;
            case UV_DIRENT_FIFO: type = "fifo"; break;
            case UV_DIRENT_SOCKET: type = "socket"; break;
            case UV_DIRENT_CHAR: type = "char"; break;
            case UV_DIRENT_BLOCK: type = "block"; break;
            default: break;
            }
            lua_pushstring(L, type);
            lua_setfield(L, -2, "type");
            lua_rawseti(L, -2, ++n);
        }
        uv_fs_req_cleanup(&req);
        return 1;
    }
    fs_start_path_async(L, UFS_SCANDIR, 2, path);
    return 1;
}

/* ---- file methods ------------------------------------------------------ */

static int l_file_read(lua_State *L) {
    uv_file_ud_t *f = check_file_open(L);
    lua_Integer size = luaL_checkinteger(L, 2);
    if (size < 0 || (uintmax_t)size > (uintmax_t)UINT_MAX)
        return luaL_error(L, "uv.file:read invalid size");

    int64_t offset = -1;
    int cb_idx = 0;
    if (lua_isfunction(L, 3)) {
        cb_idx = 3;
    } else if (!lua_isnoneornil(L, 3)) {
        check_offset(L, 3, &offset);
        if (!lua_isnoneornil(L, 4)) {
            luaL_checktype(L, 4, LUA_TFUNCTION);
            cb_idx = 4;
        }
    }

    if (!cb_idx)
        return fs_read_sync(L, f, (size_t)size, offset);

    char *buf = malloc(size ? (size_t)size : 1);
    if (!buf) return luaL_error(L, "uv.file:read out of memory");
    uv_fs_ctx_t *ctx = fs_ctx_new(L, UFS_READ, cb_idx);
    if (!ctx) {
        free(buf);
        return luaL_error(L, "uv.file:read out of memory");
    }
    ctx->file = f;
    ctx->buf = buf;
    ctx->buf_len = (size_t)size;
    file_pending_add(L, f, 1);

    uv_buf_t b = uv_buf_init(buf, (unsigned int)size);
    int rc = uv_fs_read(uv_default_loop(), &ctx->req, f->fd, &b, 1, offset, fs_cb);
    if (rc < 0) {
        file_pending_del(f);
        fs_ctx_destroy(ctx);
        return fs_fail(L, rc);
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int l_file_write(lua_State *L) {
    uv_file_ud_t *f = check_file_open(L);
    size_t len = 0;
    const char *data = luaL_checklstring(L, 2, &len);
    if (len > UINT_MAX)
        return luaL_error(L, "uv.file:write data exceeds libuv buffer limit");

    int64_t offset = -1;
    int cb_idx = 0;
    if (lua_isfunction(L, 3)) {
        cb_idx = 3;
    } else if (!lua_isnoneornil(L, 3)) {
        check_offset(L, 3, &offset);
        if (!lua_isnoneornil(L, 4)) {
            luaL_checktype(L, 4, LUA_TFUNCTION);
            cb_idx = 4;
        }
    }

    if (!cb_idx)
        return fs_write_sync(L, f, data, len, offset);

    char *buf = malloc(len ? len : 1);
    if (!buf) return luaL_error(L, "uv.file:write out of memory");
    if (len) memcpy(buf, data, len);
    uv_fs_ctx_t *ctx = fs_ctx_new(L, UFS_WRITE, cb_idx);
    if (!ctx) {
        free(buf);
        return luaL_error(L, "uv.file:write out of memory");
    }
    ctx->file = f;
    ctx->buf = buf;
    ctx->buf_len = len;
    file_pending_add(L, f, 1);

    uv_buf_t b = uv_buf_init(buf, (unsigned int)len);
    int rc = uv_fs_write(uv_default_loop(), &ctx->req, f->fd, &b, 1, offset, fs_cb);
    if (rc < 0) {
        file_pending_del(f);
        fs_ctx_destroy(ctx);
        return fs_fail(L, rc);
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int l_file_close(lua_State *L) {
    uv_file_ud_t *f = (uv_file_ud_t *)luaL_checkudata(L, 1, UV_FILE_MT);
    if (!f || f->closed) {
        lua_pushboolean(L, 0);
        return 1;
    }
    if (f->closing) {
        lua_pushnil(L);
        lua_pushstring(L, "file is already closing");
        return 2;
    }
    if (!lua_isnoneornil(L, 2))
        luaL_checktype(L, 2, LUA_TFUNCTION);

    bool has_cb = !lua_isnoneornil(L, 2);
    f->closing = true;
    if (has_cb)
        replace_ref(L, &f->close_ref, 2);

    if (f->pending_ops > 0) {
        /* Deferred: the pending callbacks will start the close. */
        lua_pushboolean(L, 1);
        return 1;
    }

    if (!has_cb) {
        if (!file_close_sync(f)) {
            f->closing = false;
            return fs_fail(L, UV_EIO);
        }
        lua_pushboolean(L, 1);
        return 1;
    }

    file_start_close_async(f, 1);
    lua_pushboolean(L, 1);
    return 1;
}

static int l_file_stat(lua_State *L) {
    uv_file_ud_t *f = check_file(L);
    if (!lua_isnoneornil(L, 2))
        luaL_checktype(L, 2, LUA_TFUNCTION);
    if (lua_isnoneornil(L, 2)) {
        uv_fs_t req;
        int rc = uv_fs_fstat(NULL, &req, f->fd, NULL);
        if (rc < 0) {
            uv_fs_req_cleanup(&req);
            return fs_fail(L, rc);
        }
        push_stat(L, &req.statbuf);
        uv_fs_req_cleanup(&req);
        return 1;
    }

    uv_fs_ctx_t *ctx = fs_ctx_new(L, UFS_FSTAT, 2);
    if (!ctx) return luaL_error(L, "uv.file:stat out of memory");
    ctx->file = f;
    file_pending_add(L, f, 1);
    int rc = uv_fs_fstat(uv_default_loop(), &ctx->req, f->fd, fs_cb);
    if (rc < 0) {
        file_pending_del(f);
        fs_ctx_destroy(ctx);
        return fs_fail(L, rc);
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int l_file_fd(lua_State *L) {
    uv_file_ud_t *f = check_file(L);
    lua_pushinteger(L, (lua_Integer)f->fd);
    return 1;
}

static int l_file_is_closing(lua_State *L) {
    uv_file_ud_t *f = (uv_file_ud_t *)luaL_checkudata(L, 1, UV_FILE_MT);
    lua_pushboolean(L, f && (f->closing || f->closed));
    return 1;
}

static int l_file_gc(lua_State *L) {
    uv_file_ud_t *f = (uv_file_ud_t *)luaL_checkudata(L, 1, UV_FILE_MT);
    if (!f) return 0;
    if (!f->closed && f->pending_ops == 0)
        file_close_sync(f);
    clear_ref(L, &f->self_ref);
    clear_ref(L, &f->close_ref);
    file_unlink(f);
    return 0;
}

/* ---- path helpers ------------------------------------------------------ */

static int l_uv_cwd(lua_State *L) {
    char buf[4096];
    size_t len = sizeof(buf);
    int rc = uv_cwd(buf, &len);
    if (rc < 0) return fs_fail(L, rc);
    lua_pushlstring(L, buf, len);
    return 1;
}

static int l_uv_chdir(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    int rc = uv_chdir(path);
    if (rc < 0) return fs_fail(L, rc);
    lua_pushboolean(L, 1);
    return 1;
}

static int l_uv_exepath(lua_State *L) {
    char buf[4096];
    size_t len = sizeof(buf);
    int rc = uv_exepath(buf, &len);
    if (rc < 0) return fs_fail(L, rc);
    lua_pushlstring(L, buf, len);
    return 1;
}

/* ---- registration ------------------------------------------------------ */

static const luaL_Reg uv_fs_lib[] = {
    {"fs_stat", l_fs_stat},
    {"fs_lstat", l_fs_lstat},
    {"fs_open", l_fs_open},
    {"fs_unlink", l_fs_unlink},
    {"fs_mkdir", l_fs_mkdir},
    {"fs_rmdir", l_fs_rmdir},
    {"fs_rename", l_fs_rename},
    {"fs_copyfile", l_fs_copyfile},
    {"fs_chmod", l_fs_chmod},
    {"fs_chown", l_fs_chown},
    {"fs_utime", l_fs_utime},
    {"fs_realpath", l_fs_realpath},
    {"fs_scandir", l_fs_scandir},
    {"cwd", l_uv_cwd},
    {"chdir", l_uv_chdir},
    {"exepath", l_uv_exepath},
    {NULL, NULL}
};

static const luaL_Reg uv_file_methods[] = {
    {"read", l_file_read},
    {"write", l_file_write},
    {"close", l_file_close},
    {"stat", l_file_stat},
    {"fd", l_file_fd},
    {"is_closing", l_file_is_closing},
    {NULL, NULL}
};

void tmg_uv_fs_register(lua_State *L) {
    if (luaL_newmetatable(L, UV_FILE_MT)) {
        lua_pushcfunction(L, l_file_gc);
        lua_setfield(L, -2, "__gc");
        lua_newtable(L);
        luaL_register(L, NULL, uv_file_methods);
        lua_setfield(L, -2, "__index");
    }
    lua_pop(L, 1);

    for (const luaL_Reg *r = uv_fs_lib; r->name; r++) {
        lua_pushcfunction(L, r->func);
        lua_setfield(L, -2, r->name);
    }
}

void tmg_uv_fs_shutdown(lua_State *L) {
    (void)L;

    uv_fs_ctx_t *ctx = g_fs_ctxs;
    while (ctx) {
        uv_fs_ctx_t *next = ctx->next;
        ctx->shutdown = true;
        clear_ref(ctx->L, &ctx->cb_ref);
        uv_cancel((uv_req_t *)&ctx->req);
        ctx = next;
    }

    uv_file_ud_t *f = g_files;
    while (f) {
        clear_ref(f->L, &f->close_ref);
        if (!f->closed && f->pending_ops == 0) {
            f->closing = true;
            file_close_sync(f);
        } else {
            f->closing = true;
        }
        f = f->next;
    }
}
