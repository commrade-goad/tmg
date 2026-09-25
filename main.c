#include <luajit.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <stdbool.h>
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

#include "argparse.h"
#include "str.h"

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

    /* Find maximum consecutive '=' between ']' and ']' in buffer to prevent delimiter collision */
    int max_equals = 0;
    for (size_t i = 0; i < buffer->len; i++) {
        if (buffer->data[i] == ']') {
            int equals = 0;
            size_t j = i + 1;
            while (j < buffer->len && buffer->data[j] == '=') {
                equals++;
                j++;
            }
            if (j < buffer->len && buffer->data[j] == ']') {
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
    for (size_t i = 0; i < buffer->len; i++) {
        char ch = buffer->data[i];
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

static bool tmg_expand(lua_State *L, const char *in_path, const char *sep, bool debug)
{
    FILE *in = fopen(in_path, "r");
    if (!in) {
        fprintf(stderr, "ERR: Failed to read the input file `%s`: %s\n",
                in_path, strerror(errno));
        return false;
    }
    fseek(in, 0, SEEK_END);
    long in_size = ftell(in);
    rewind(in);

    size_t sepsize = strlen(sep);
    str_t match_buf = str_init(sepsize + 1);
    str_t builder   = str_init(in_size * 2);
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
        report_error_with_code(lua_tostring(L, -1), in_path, &lm, builder.data);
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

    str_t final_output = str_init(in_size * 2);
    bool success = false;

    int status = lua_resume(co, 0);

    while (status == LUA_YIELD) {
        int top = lua_gettop(co);
        for (int i = 1; i <= top; i++) {
            if (lua_isstring(co, i)) {
                size_t len;
                const char *chunk = lua_tolstring(co, i, &len);
                str_push(&final_output, chunk);
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
        report_error_with_code(lua_tostring(co, -1), in_path, &lm, builder.data);
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
    if (!tmg_expand(L, in_path, sep, debug)) return false;

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

bool tmg_render_string(lua_State *L, const char *in_path, const char *sep, bool debug)
{
    return tmg_expand(L, in_path, sep, debug);
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

    if (!lua_isstring(L, 1)) {
        lua_pushstring(L, lua_tolstring(L, 1, NULL));
        lua_replace(L, 1);
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

    if (tmg_render_string(L, in_path, sep, debug)) {
        return 1;
    }
    lua_pushnil(L);
    return 1;
}

static const luaL_Reg tmg_lib[] = {
    {"render", l_tmg_render},
    {"render_string", l_tmg_render_string},
    {"print", l_tmg_print},
    {NULL, NULL}
};

int luaopen_tmg(lua_State *L) {
    luaL_newlib(L, tmg_lib);
    return 1;
}

int main(int argc, char **argv)
{
    struct popt *opt = parse_args(argc, argv);
    if (!opt) return EXIT_FAILURE;
    if (opt->exit) { cleanup_args(opt); return EXIT_SUCCESS; }

    if (!opt->script) {
        fprintf(stderr, "ERR: no build script given.\n");
        fprintf(stderr, "Usage: %s <script.lua>\n", argv[0]);
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

    luaL_requiref(L, "tmg", luaopen_tmg, 1);
    lua_pop(L, 1);

    int exit_code = EXIT_SUCCESS;

    lua_pushcfunction(L, msghandler);
    int msgh_idx = lua_gettop(L);

    if (luaL_loadfile(L, opt->script) != LUA_OK) {
        fprintf(stderr, "ERR: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
        exit_code = EXIT_FAILURE;
    } else if (lua_pcall(L, 0, 0, msgh_idx) != LUA_OK) {
        fprintf(stderr, "ERR: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
        exit_code = EXIT_FAILURE;
    }
    lua_remove(L, msgh_idx);

    lua_close(L);
    cleanup_args(opt);
    return exit_code;
}
