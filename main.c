#include <luajit.h>
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

#include "argparse.h"
#include "http.h"
#include "str.h"

#define TMG_RAW_MT "tmg.raw"

typedef struct {
    size_t len;
    char data[];
} tmg_raw_t;

static tmg_raw_t *tmg_raw_check(lua_State *L, int idx) {
    if (!lua_getmetatable(L, idx))
        return NULL;
    luaL_getmetatable(L, TMG_RAW_MT);
    bool same = lua_rawequal(L, -1, -2) != 0;
    lua_pop(L, 2);
    return same ? (tmg_raw_t *)lua_touserdata(L, idx) : NULL;
}

static void tmg_push_raw(lua_State *L, const char *s, size_t len) {
    tmg_raw_t *raw = (tmg_raw_t *)lua_newuserdata(L, sizeof(*raw) + len + 1);
    raw->len = len;
    if (len) memcpy(raw->data, s, len);
    raw->data[len] = '\0';
    luaL_getmetatable(L, TMG_RAW_MT);
    lua_setmetatable(L, -2);
}

static void tmg_push_escaped(str_t *out, const char *s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        switch ((unsigned char)s[i]) {
        case '&': str_push(out, "&amp;"); break;
        case '<': str_push(out, "&lt;"); break;
        case '>': str_push(out, "&gt;"); break;
        case '"': str_push(out, "&quot;"); break;
        case '\'': str_push(out, "&#39;"); break;
        default: str_push_chr(out, s[i]); break;
        }
    }
}

#ifndef luaL_requiref
static void luaL_requiref(lua_State *L, const char *modname, lua_CFunction openf, int glb) {
    lua_pushcfunction(L, openf);
    lua_pushstring(L, modname);
    lua_call(L, 1, 1); /* call openf(modname) */

    /* Get registry["_LOADED"] */
    lua_getfield(L, LUA_REGISTRYINDEX, "_LOADED");
    if (lua_istable(L, -1)) {
        lua_pushvalue(L, -2);          /* push module copy */
        lua_setfield(L, -2, modname);  /* _LOADED[modname] = module */
    }
    lua_pop(L, 1); /* pop _LOADED table */

    if (glb) {
        lua_pushvalue(L, -1);          /* push module copy */
        lua_setglobal(L, modname);     /* _G[modname] = module */
    }
}
#endif

/* ---- generated-line -> original-line map ------------------------------ */

typedef struct {
    int *data;
    size_t len, cap;
} linemap_t;

static linemap_t linemap_init(void) {
    linemap_t lm = { .data = malloc(sizeof(int) * 64), .len = 0, .cap = 64 };
    return lm;
}

static void linemap_deinit(linemap_t *lm) {
    free(lm->data);
    lm->data = NULL;
    lm->len = lm->cap = 0;
}

static void linemap_push(linemap_t *lm, int orig_line) {
    if (lm->len >= lm->cap) {
        lm->cap *= 2;
        lm->data = realloc(lm->data, sizeof(int) * lm->cap);
    }
    lm->data[lm->len++] = orig_line;
}

/* call every time a real '\n' byte is appended to the generated chunk */
static void note_newline(linemap_t *lm, int *gen_line, int orig_line) {
    (*gen_line)++;
    linemap_push(lm, orig_line);
}

/* ---- error reporting ---------------------------------------------------- */

static int msghandler(lua_State *L) {
    const char *msg = lua_tostring(L, -1);
    if (msg == NULL) {
        if (luaL_callmeta(L, 1, "__tostring") && lua_type(L, -1) == LUA_TSTRING)
            return 1;
        msg = lua_pushfstring(L, "(error object is a %s value)", luaL_typename(L, 1));
    }
    luaL_traceback(L, L, msg, 1);
    return 1;
}

static void report_error_with_code(const char *msg, const char *in_path,
                                   const linemap_t *lm, const char *gen_code)
{
    if (!msg) msg = "(unknown error)";

    char prefix[1024];
    snprintf(prefix, sizeof(prefix), "%s:", in_path);
    const char *p = strstr(msg, prefix);

    int gen_line = -1;
    int orig_line = -1;
    const char *err_detail = msg;

    if (p) {
        p += strlen(prefix);
        char *rest = NULL;
        long parsed_line = strtol(p, &rest, 10);
        if (rest != p) {
            gen_line = (int)parsed_line;
            if (gen_line > 0 && (size_t)gen_line <= lm->len) {
                orig_line = lm->data[gen_line - 1];
            } else {
                orig_line = gen_line;
            }
            if (*rest == ':') rest++;
            err_detail = rest;
        }
    }

    /* Print main error header */
    if (gen_line != -1) {
        fprintf(stderr, "ERR: %s:%d:%s\n\n", in_path, orig_line, err_detail);
    } else {
        fprintf(stderr, "ERR: %s\n\n", msg);
    }

    /* Print context snippet from generated code if available */
    if (gen_line > 0 && gen_code) {
        fprintf(stderr, "--- Generated Code Context ---\n");

        int current_line = 1;
        const char *line_start = gen_code;
        const char *ptr = gen_code;

        while (*ptr != '\0') {
            if (*ptr == '\n' || *(ptr + 1) == '\0') {
                size_t line_len = ptr - line_start + (*ptr == '\n' ? 0 : 1);

                if (current_line >= gen_line - 3 && current_line <= gen_line + 3) {
                    char marker = (current_line == gen_line) ? '>' : ' ';
                    fprintf(stderr, "%c %4d | %.*s\n", marker, current_line, (int)line_len, line_start);
                }

                line_start = ptr + 1;
                current_line++;
            }
            ptr++;
        }
        fprintf(stderr, "------------------------------\n");
    }
}

/* Safely flushes accumulated text as yield([==[...]==]) with dynamic equal sign counting */
static void flush_text_buffer(str_t *builder, str_t *buffer, linemap_t *lm, int *gen_line, int cur_line) {
    if (buffer->len == 0) return;

    /* Lua long strings skip a newline immediately after the opening bracket.
     * Emit any leading newlines as an escaped quoted string first. */
    size_t start = 0;
    while (start < buffer->len && buffer->data[start] == '\n')
        start++;
    if (start > 0) {
        str_push(builder, "tmg.print(\"");
        for (size_t i = 0; i < start; i++)
            str_push(builder, "\\n");
        str_push(builder, "\")\n");
        note_newline(lm, gen_line, cur_line);
    }
    if (start == buffer->len) {
        str_clear(buffer);
        return;
    }

    const char *body = buffer->data + start;
    size_t body_len = buffer->len - start;

    /* Find maximum consecutive '=' between ']' and ']' in buffer to prevent delimiter collision */
    int max_equals = 0;
    for (size_t i = 0; i < body_len; i++) {
        if (body[i] == ']') {
            int equals = 0;
            size_t j = i + 1;
            while (j < body_len && body[j] == '=') {
                equals++;
                j++;
            }
            if (j < body_len && body[j] == ']') {
                if (equals >= max_equals) {
                    max_equals = equals + 1;
                }
            }
        }
    }

    /* Build opening long bracket: yield([==...==[ */
    str_push(builder, "tmg.print([");
    for (int i = 0; i < max_equals; i++) str_push_chr(builder, '=');
    str_push_chr(builder, '[');

    /* Push body content verbatim and track line map alignment */
    for (size_t i = 0; i < body_len; i++) {
        char ch = body[i];
        str_push_chr(builder, ch);
        if (ch == '\n') {
            note_newline(lm, gen_line, cur_line);
        }
    }

    /* Build closing long bracket: ]==...==]) */
    str_push_chr(builder, ']');
    for (int i = 0; i < max_equals; i++) str_push_chr(builder, '=');
    str_push(builder, "])\n");
    note_newline(lm, gen_line, cur_line);
    str_clear(buffer);
}

/* ---- core expand ---------------------------------------------------------- */

static void tmg_set_error(str_t *err, const char *msg) {
    if (!err) return;
    str_clear(err);
    str_push(err, msg ? msg : "(unknown error)");
}

static bool tmg_expand(lua_State *L, const char *in_path, const char *sep, bool debug,
                       int data_idx, bool html, str_t *err)
{
    FILE *in = fopen(in_path, "r");
    if (!in) {
        char msg[1024];
        snprintf(msg, sizeof(msg), "failed to read `%s`: %s", in_path, strerror(errno));
        fprintf(stderr, "ERR: %s\n", msg);
        tmg_set_error(err, msg);
        return false;
    }
    fseek(in, 0, SEEK_END);
    long file_size = ftell(in);
    rewind(in);
    size_t in_size = file_size > 0 ? (size_t)file_size : 0;
    int initial_cap = 16;
    if (in_size > 0 && in_size <= (size_t)INT_MAX / 2)
        initial_cap = (int)(in_size * 2);

    size_t sepsize = strlen(sep);
    str_t match_buf = str_init(sepsize + 1);
    str_t builder   = str_init(initial_cap);
    str_t buffer    = str_init(100);
    linemap_t lm    = linemap_init();
    int gen_line    = 1;
    int cur_line    = 1;
    linemap_push(&lm, cur_line);

    bool code = false;
    bool escape_next = false;
    int c;

    while ((c = fgetc(in)) != EOF) {
        if (c == '\n') cur_line++;

        if (escape_next) {
            escape_next = false;
            if (code) {
                /* Code mode: '\' only suppresses delimiter start; otherwise keep '\' */
                bool suppresses_delim = sepsize > 0 && (unsigned char)c == (unsigned char)sep[0];
                if (!suppresses_delim) str_push_chr(&builder, '\\');
                str_push_chr(&builder, (char)c);
                if (c == '\n') note_newline(&lm, &gen_line, cur_line);
            } else if (c == '\n') {
                /* Text mode: '\<newline>' line continuation swallower */
            } else {
                /* Text mode: If '\' wasn't suppressing a delimiter, pass '\' as raw text */
                bool suppresses_delim = sepsize > 0 && (unsigned char)c == (unsigned char)sep[0];
                if (!suppresses_delim) str_push_chr(&buffer, '\\');
                str_push_chr(&buffer, (char)c);
            }
            continue;
        }

        if (c == '\\') {
            escape_next = true;
            continue;
        }

        str_push_chr(&match_buf, (char)c);

        if (match_buf.len <= sepsize && strncmp(match_buf.data, sep, match_buf.len) == 0) {
            if (match_buf.len == sepsize) {
                str_push_chr(&builder, '\n');
                note_newline(&lm, &gen_line, cur_line);

                code = !code;

                flush_text_buffer(&builder, &buffer, &lm, &gen_line, cur_line);
                str_clear(&match_buf);
            }
            continue;
        }

        for (size_t i = 0; i < match_buf.len; i++) {
            char ch = match_buf.data[i];
            if (code) {
                str_push_chr(&builder, ch);
                if (ch == '\n') note_newline(&lm, &gen_line, cur_line);
            } else {
                str_push_chr(&buffer, ch);
            }
        }
        str_clear(&match_buf);
    }
    fclose(in);

    if (escape_next) {
        if (code) str_push_chr(&builder, '\\');
        else str_push_chr(&buffer, '\\');
    }

    if (match_buf.len > 0) {
        for (size_t i = 0; i < match_buf.len; i++) {
            char ch = match_buf.data[i];
            if (code) str_push_chr(&builder, ch);
            else str_push_chr(&buffer, ch);
        }
        str_clear(&match_buf);
    }

    flush_text_buffer(&builder, &buffer, &lm, &gen_line, cur_line);

    if (debug) {
        fprintf(stderr, "-- generated chunk for %s --\n%s-- end --\n",
                in_path, builder.data);
    }

    /* ---- Coroutine Execution Engine ---- */

    char chunkname[1024];
    snprintf(chunkname, sizeof(chunkname), "@%s", in_path);

    if (luaL_loadbuffer(L, builder.data, builder.len, chunkname) != LUA_OK) {
        const char *msg = lua_tostring(L, -1);
        tmg_set_error(err, msg);
        report_error_with_code(msg, in_path, &lm, builder.data);
        lua_pop(L, 1);
        str_deinit(&builder);
        str_deinit(&buffer);
        str_deinit(&match_buf);
        linemap_deinit(&lm);
        return false;
    }

    /* Create thread context */
    lua_State *co = lua_newthread(L);
    lua_pushvalue(L, -2); /* duplicate chunk function */
    lua_xmove(L, co, 1);  /* move function onto thread stack */

    /* Per-request data is passed as the template chunk's vararg, so
     * templates can use `local data = ...` without touching globals. */
    int nargs = 0;
    if (data_idx > 0) {
        lua_pushvalue(L, data_idx);
        lua_xmove(L, co, 1);
        nargs = 1;
    }

    str_t final_output = str_init(initial_cap);
    bool success = false;

    int status = lua_resume(co, nargs);

    while (status == LUA_YIELD) {
        int top = lua_gettop(co);
        for (int i = 1; i <= top; i++) {
            if (lua_isstring(co, i)) {
                size_t len;
                const char *chunk = lua_tolstring(co, i, &len);
                if (html)
                    tmg_push_escaped(&final_output, chunk, len);
                else
                    str_push_len(&final_output, chunk, len);
            } else {
                tmg_raw_t *raw = tmg_raw_check(co, i);
                if (raw)
                    str_push_len(&final_output, raw->data, raw->len);
            }
        }
        lua_pop(co, top);

        status = lua_resume(co, 0);
    }

    if (status == LUA_OK) {
        /* Clean thread and chunk function off main stack BEFORE pushing result */
        lua_pop(L, 2); /* pops 'co' and loaded chunk function */

        /* Push result string - now it is strictly on top of stack L */
        lua_pushlstring(L, final_output.data, final_output.len);
        success = true;
    } else {
        /* Runtime error during coroutine streaming */
        const char *msg = lua_tostring(co, -1);
        tmg_set_error(err, msg);
        report_error_with_code(msg, in_path, &lm, builder.data);
        lua_pop(L, 2); /* pop thread and function on error */
    }

    str_deinit(&final_output);
    str_deinit(&builder);
    str_deinit(&buffer);
    str_deinit(&match_buf);
    linemap_deinit(&lm);

    return success;
}

/* Parses+runs the template and writes the rendered result to out_path via C
* stdio directly (never through Lua's io library). */
bool tmg_render(lua_State *L, const char *in_path, const char *out_path,
                const char *sep, bool debug)
{
    if (!tmg_expand(L, in_path, sep, debug, 0, false, NULL)) return false;

    const char *result = lua_tostring(L, -1);
    bool success = false;
    FILE *out = fopen(out_path, "w");
    if (!out) {
        fprintf(stderr, "ERR: Failed to create the output file `%s`: %s\n",
                out_path, strerror(errno));
    } else {
        fputs(result, out);
        fclose(out);
        success = true;
    }
    lua_pop(L, 1);
    return success;
}

bool tmg_render_string(lua_State *L, const char *in_path, const char *sep, bool debug,
                       bool html)
{
    return tmg_expand(L, in_path, sep, debug, 0, html, NULL);
}

/* ---- TMG_LIB_PATH ---------------------------------------------------------- */

#define LIBDIRS_KEY "tmg.libdirs"

#if LUA_VERSION_NUM >= 502
#define tmg_objlen(L, i) ((int)lua_rawlen((L), (i)))
#else
#define tmg_objlen(L, i) ((int)lua_objlen((L), (i)))
#endif

/* TMG_LIB_PATH=dir1:dir2 -> every dir is added to package.path (.lua files,
 * and dir/name/init.lua) and package.cpath (.so modules). Entries are
 * prepended in the order given, so they win over the system defaults and
 * dir1 wins over dir2. Empty entries are ignored.
 *
 * The parsed dirs are also kept in the registry (see tmg.find_lib and
 * tmg.lib_path), even when the variable is unset (then the list is empty). */
static void setup_lib_path(lua_State *L)
{
    const char *env = getenv("TMG_LIB_PATH");

    lua_newtable(L);
    int dirs = lua_gettop(L);
    int n = 0;

    str_t lpath = str_init(256);
    str_t cpath = str_init(128);

    for (const char *p = env; p && *p; ) {
        const char *end = strchr(p, ':');
        size_t len = end ? (size_t)(end - p) : strlen(p);

        if (len > 0) {
            /* drop a trailing '/' so we don't produce "dir//?.lua" */
            while (len > 1 && p[len - 1] == '/') len--;

            char *dir = malloc(len + 1);
            memcpy(dir, p, len);
            dir[len] = '\0';

            lua_pushlstring(L, dir, len);
            lua_rawseti(L, dirs, ++n);

            str_push(&lpath, dir); str_push(&lpath, "/?.lua;");
            str_push(&lpath, dir); str_push(&lpath, "/?/init.lua;");
            str_push(&cpath, dir); str_push(&cpath, "/?.so;");
            free(dir);
        }
        if (!end) break;
        p = end + 1;
    }

    lua_pushvalue(L, dirs);
    lua_setfield(L, LUA_REGISTRYINDEX, LIBDIRS_KEY);

    lua_getglobal(L, "package");
    if (n > 0 && lua_istable(L, -1)) {
        lua_pushlstring(L, lpath.data, lpath.len);
        lua_getfield(L, -2, "path");
        lua_concat(L, 2);                 /* ours .. old */
        lua_setfield(L, -2, "path");

        lua_pushlstring(L, cpath.data, cpath.len);
        lua_getfield(L, -2, "cpath");
        lua_concat(L, 2);
        lua_setfield(L, -2, "cpath");
    }
    lua_pop(L, 2); /* package, dirs */

    str_deinit(&lpath);
    str_deinit(&cpath);
}

/* tmg.find_lib(name) -> path | nil, err
 *
 * Looks for a shared library in every TMG_LIB_PATH dir, in order, and returns
 * the first match, ready to hand to ffi.load():
 *
 *   find_lib("raylib")         -> <dir>/libraylib.so, then <dir>/raylib.so
 *   find_lib("native/raylib")  -> <dir>/native/libraylib.so, ...
 *   find_lib("libfoo.so.2")    -> <dir>/libfoo.so.2        (explicit file name)
 *   find_lib("/abs/libx.so")   -> returned as is if it exists
 */
static int l_tmg_find_lib(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);

    /* split "sub/dir/base" into "sub/dir/" and "base" */
    const char *slash = strrchr(name, '/');
    size_t sublen = slash ? (size_t)(slash - name) + 1 : 0;
    const char *base = name + sublen;
    char sub[1024];
    if (sublen >= sizeof(sub)) return luaL_error(L, "find_lib: name too long");
    memcpy(sub, name, sublen);
    sub[sublen] = '\0';

    if (name[0] == '/') {
        if (access(name, R_OK) == 0) { lua_pushstring(L, name); return 1; }
        lua_pushnil(L);
        lua_pushfstring(L, "library '%s' not found", name);
        return 2;
    }

    if (*base == '\0') return luaL_error(L, "find_lib: empty library name");

    /* a name that already looks like a file ("foo.so", "libfoo.so.2") is
     * taken literally; a bare name gets the lib<name>.so / <name>.so forms */
    bool literal = strstr(base, ".so") != NULL;
    const char *fmts[2] = { literal ? "%s/%s%s" : "%s/%slib%s.so",
                            literal ? NULL     : "%s/%s%s.so" };

    lua_getfield(L, LUA_REGISTRYINDEX, LIBDIRS_KEY);
    int n = lua_istable(L, -1) ? tmg_objlen(L, -1) : 0;

    str_t tried = str_init(256);
    char cand[4096];

    for (int i = 1; i <= n; i++) {
        lua_rawgeti(L, -1, i);
        const char *dir = lua_tostring(L, -1);
        for (int f = 0; f < 2 && fmts[f]; f++) {
            int w = snprintf(cand, sizeof(cand), fmts[f], dir, sub, base);
            if (w < 0 || (size_t)w >= sizeof(cand)) continue;
            if (access(cand, R_OK) == 0) {
                lua_pushstring(L, cand);
                str_deinit(&tried);
                return 1;
            }
            str_push(&tried, "\n\tno file '");
            str_push(&tried, cand);
            str_push_chr(&tried, '\'');
        }
        lua_pop(L, 1);
    }

    lua_pushnil(L);
    if (n == 0)
        lua_pushfstring(L, "library '%s' not found (TMG_LIB_PATH is empty)", name);
    else
        lua_pushfstring(L, "library '%s' not found in TMG_LIB_PATH:%s",
                        name, tried.data ? tried.data : "");
    str_deinit(&tried);
    return 2;
}

/* ---- Lua-facing glue ------------------------------------------------- */

static const char *resolve_sep(lua_State *L, int idx) {
    const char *sep = "%";
    if (!lua_isnoneornil(L, idx)) {
        const char *s = luaL_checkstring(L, idx);
        if (s) sep = s;
    } else {
        lua_getglobal(L, "tmg_delim");
        if (lua_isstring(L, -1)) {
            const char *s = lua_tostring(L, -1);
            if (s) sep = s;
        }
        lua_pop(L, 1);
    }
    return sep;
}

static bool resolve_debug(lua_State *L) {
    bool debug = false;
    lua_getglobal(L, "tmg_debug");
    if (lua_toboolean(L, -1)) debug = true;
    lua_pop(L, 1);
    return debug;
}

static int l_tmg_print(lua_State *L) {
    luaL_checkany(L, 1);
    lua_settop(L, 1);

    if (tmg_raw_check(L, 1))
        return lua_yield(L, 1);

    if (!lua_isstring(L, 1)) {
        if (luaL_callmeta(L, 1, "__tostring")) {
            /* objects with __tostring (e.g. latex nodes) print as that string */
            lua_replace(L, 1);
            lua_settop(L, 1);
        } else {
            lua_pushstring(L, lua_tolstring(L, 1, NULL));
            lua_replace(L, 1);
        }
    }

    return lua_yield(L, 1);
}

static int l_tmg_render(lua_State *L) {
    const char *in_path  = luaL_checkstring(L, 1);
    const char *out_path = luaL_checkstring(L, 2);
    const char *sep = resolve_sep(L, 3);
    bool debug = resolve_debug(L);

    bool ok = tmg_render(L, in_path, out_path, sep, debug);
    lua_pushboolean(L, ok);
    return 1;
}

static int l_tmg_render_string(lua_State *L) {
    const char *in_path = luaL_checkstring(L, 1);
    const char *sep = resolve_sep(L, 2);
    bool debug = resolve_debug(L);
    int data_idx = lua_isnoneornil(L, 3) ? 0 : 3;
    bool html = false;

    if (lua_isboolean(L, 4)) {
        html = lua_toboolean(L, 4) != 0;
    } else if (lua_istable(L, 4)) {
        lua_getfield(L, 4, "autoescape");
        if (!lua_isnil(L, -1)) {
            html = lua_toboolean(L, -1) != 0;
        } else {
            lua_pop(L, 1);
            lua_getfield(L, 4, "html");
            if (!lua_isnil(L, -1)) html = lua_toboolean(L, -1) != 0;
        }
        lua_pop(L, 1);
    }

    str_t err = str_init(64);
    if (tmg_expand(L, in_path, sep, debug, data_idx, html, &err)) {
        str_deinit(&err);
        return 1;
    }
    lua_pushnil(L);
    lua_pushlstring(L, err.data, err.len);
    str_deinit(&err);
    return 2;
}

static int l_tmg_raw_tostring(lua_State *L) {
    tmg_raw_t *raw = (tmg_raw_t *)lua_touserdata(L, 1);
    if (!raw) return 0;
    lua_pushlstring(L, raw->data, raw->len);
    return 1;
}

static int l_tmg_escape_html(lua_State *L) {
    tmg_raw_t *raw = tmg_raw_check(L, 1);
    size_t len = 0;
    const char *s = raw ? raw->data : luaL_checklstring(L, 1, &len);
    if (raw) len = raw->len;

    str_t out = str_init(len + 16);
    tmg_push_escaped(&out, s, len);
    lua_pushlstring(L, out.data, out.len);
    str_deinit(&out);
    return 1;
}

static int l_tmg_raw(lua_State *L) {
    tmg_raw_t *raw = tmg_raw_check(L, 1);
    if (raw) {
        lua_pushvalue(L, 1);
        return 1;
    }
    size_t len = 0;
    const char *s = luaL_checklstring(L, 1, &len);
    tmg_push_raw(L, s, len);
    return 1;
}

static const luaL_Reg tmg_lib[] = {
    {"render", l_tmg_render},
    {"render_string", l_tmg_render_string},
    {"print", l_tmg_print},
    {"escape_html", l_tmg_escape_html},
    {"raw", l_tmg_raw},
    {"find_lib", l_tmg_find_lib},
    {NULL, NULL}
};

int luaopen_tmg(lua_State *L) {
    luaL_newmetatable(L, TMG_RAW_MT);
    lua_pushcfunction(L, l_tmg_raw_tostring);
    lua_setfield(L, -2, "__tostring");
    lua_pop(L, 1);

    luaL_newlib(L, tmg_lib);
    lua_getfield(L, LUA_REGISTRYINDEX, LIBDIRS_KEY);
    if (lua_istable(L, -1)) lua_setfield(L, -2, "lib_path");
    else lua_pop(L, 1);
    return 1;
}

/* Standard Lua convention: global `arg` with arg[0] = script, arg[1..n] = the
 * script's arguments and arg[-1] = the interpreter (negative indices go back
 * through argv). The same arguments are also passed to the main chunk as `...`. */
static void set_arg_table(lua_State *L, int argc, char **argv, int script_idx)
{
    lua_createtable(L, argc - script_idx - 1, script_idx + 1);
    for (int i = 0; i < argc; i++) {
        lua_pushstring(L, argv[i]);
        lua_rawseti(L, -2, i - script_idx);
    }
    lua_setglobal(L, "arg");
}

int main(int argc, char **argv)
{
    struct popt *opt = parse_args(argc, argv);
    if (!opt) return EXIT_FAILURE;
    if (opt->exit) { cleanup_args(opt); return EXIT_SUCCESS; }

    if (!opt->script) {
        fprintf(stderr, "ERR: no build script given.\n");
        fprintf(stderr, "Usage: %s <script.lua> [args...]\n", argv[0]);
        cleanup_args(opt);
        return EXIT_FAILURE;
    }

    lua_State *L = luaL_newstate();
    if (L == NULL) {
        fprintf(stderr, "ERR: Failed to create Lua state.\n");
        cleanup_args(opt);
        return EXIT_FAILURE;
    }
    luaL_openlibs(L);
    setup_lib_path(L);
    set_arg_table(L, argc, argv, opt->script_idx);

    luaL_requiref(L, "tmg", luaopen_tmg, 1);
    lua_pop(L, 1);

    luaL_requiref(L, "http", luaopen_http, 1);
    lua_pop(L, 1);

    int exit_code = EXIT_SUCCESS;

    lua_pushcfunction(L, msghandler);
    int msgh_idx = lua_gettop(L);

    if (luaL_loadfile(L, opt->script) != LUA_OK) {
        fprintf(stderr, "ERR: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
        exit_code = EXIT_FAILURE;
    } else {
        /* the script's arguments are also the varargs of its main chunk */
        int nargs = argc - opt->script_idx - 1;
        luaL_checkstack(L, nargs, "too many script arguments");
        for (int i = opt->script_idx + 1; i < argc; i++)
            lua_pushstring(L, argv[i]);

        if (lua_pcall(L, nargs, 0, msgh_idx) != LUA_OK) {
            fprintf(stderr, "ERR: %s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
            exit_code = EXIT_FAILURE;
        } else {
            http_state_run(L);
        }
    }
    http_state_deinit(L);
    lua_remove(L, msgh_idx);

    lua_close(L);
    cleanup_args(opt);
    return exit_code;
}
