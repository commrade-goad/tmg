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

static bool only_whitespace(char *str) {
    char *ptr = str;
    while (ptr && *ptr != '\0') {
        if (!isspace((unsigned char)*ptr++)) return false;
    }
    return true;
}

/* ---- error reporting ----------------------------------------------------
 * Lua error messages look like "<chunkname>:<gen_line>: <msg>". gen_line
 * refers to the synthesized chunk we built, not the user's .tmg file, so we
 * rewrite it using the line map before printing. Best-effort: several
 * source lines collapsing onto one generated line will point at the first
 * of them. */
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

static void report_error(const char *msg, const char *in_path, linemap_t *lm) {
    if (!msg) msg = "(unknown error)";

    char prefix[1024];
    snprintf(prefix, sizeof(prefix), "%s:", in_path);
    const char *p = strstr(msg, prefix);

    if (p) {
        p += strlen(prefix);
        char *rest = NULL;
        long gen_line = strtol(p, &rest, 10);
        if (rest != p) {
            int orig_line = (int)gen_line;
            if (gen_line > 0 && (size_t)gen_line <= lm->len)
                orig_line = lm->data[gen_line - 1];
            if (*rest == ':') rest++;
            fprintf(stderr, "ERR: %s:%d:%s\n", in_path, orig_line, rest);
            return;
        }
    }
    fprintf(stderr, "ERR: %s\n", msg);
}

/* ---- core expand ----------------------------------------------------------
 * Parses `in_path` as a .tmg template, builds a synthetic Lua chunk, and
 * runs it in the caller's lua_State L (so it sees whatever the build script
 * already `require`d / set up). On success leaves the rendered string on
 * top of the Lua stack and returns true. On failure the stack is left as it
 * was, an error is reported to stderr, and false is returned. This is the
 * shared core behind both tmg_render (writes to a file) and
 * tmg_render_string (hands the string back to the caller); it never touches
 * Lua's stdout/stderr itself, so a build script's own logging can never
 * collide with template output. */
static bool tmg_expand(lua_State *L, const char *in_path, char sep, bool debug)
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

    str_t builder = str_init(in_size * 2);
    str_t buffer  = str_init(100);
    linemap_t lm  = linemap_init();
    int gen_line  = 1;
    int cur_line  = 1;
    linemap_push(&lm, cur_line); /* gen line 1 -> orig line 1 */

    str_push(&builder, "local out = {}\n");
    note_newline(&lm, &gen_line, cur_line);

    bool code = false;
    bool inline_code = false;
    int c;
    while ((c = fgetc(in)) != EOF) {
        if (c == '\n') cur_line++;

        if (c == sep) {
            if (inline_code) str_push(&builder, ")");
            str_push_chr(&builder, '\n');
            note_newline(&lm, &gen_line, cur_line);

            inline_code = false;
            code = !code;

            if (buffer.len > 0) {
                if (only_whitespace(buffer.data)) {
                    str_clear(&buffer);
                    continue;
                }
                str_push(&builder, "out[#out+1] = \"");
                str_push(&builder, buffer.data);
                str_push(&builder, "\"\n");
                note_newline(&lm, &gen_line, cur_line);
                str_clear(&buffer);
            }
            continue;
        }
        if (code) {
            if (c == '\\') {
                int next = fgetc(in);
                if (next == '\n') cur_line++;
                if (!isspace(next))
                    str_push_chr(&builder, next);
                continue;
            }
            if (c == '$') {
                inline_code = true;
                c = fgetc(in);
                if (c == '\n') cur_line++;
                str_push(&builder, "out[#out+1] = ");
                str_push(&builder, "tostring(");
            }
            str_push_chr(&builder, c);
            if (c == '\n') note_newline(&lm, &gen_line, cur_line);
            continue;
        } else {
            if (c == '\\') {
                int next = fgetc(in);
                if (next == '\n') cur_line++;
                if (!isspace(next))
                    str_push_chr(&buffer, next);
                continue;
            }
            if (isspace(c) || c == '"') {
                switch (c) {
                    case '\n':
                        str_push_chr(&buffer, '\\');
                        str_push_chr(&buffer, 'n');
                        break;
                    case '\r':
                        str_push_chr(&buffer, '\\');
                        str_push_chr(&buffer, 'r');
                        break;
                    case '\t':
                        str_push_chr(&buffer, '\\');
                        str_push_chr(&buffer, 't');
                        break;
                    case '"':
                        str_push_chr(&buffer, '\\');
                        str_push_chr(&buffer, '"');
                        break;
                    default:
                        str_push_chr(&buffer, c);
                        break;
                }
                continue;
            }
            str_push_chr(&buffer, c);
        }
    }
    fclose(in);

    if (buffer.len > 0) {
        str_push(&builder, "out[#out+1] = \"");
        str_push(&builder, buffer.data);
        str_push(&builder, "\"\n");
        note_newline(&lm, &gen_line, cur_line);
        str_clear(&buffer);
    }

    str_push(&builder, "return table.concat(out)\n");
    note_newline(&lm, &gen_line, cur_line);
    /* end parsing */

    if (debug) {
        fprintf(stderr, "-- generated chunk for %s --\n%s-- end --\n",
                in_path, builder.data);
    }

    bool success = false;

    lua_pushcfunction(L, msghandler);
    int msgh_idx = lua_gettop(L);

    char chunkname[1024];
    snprintf(chunkname, sizeof(chunkname), "@%s", in_path);

    if (luaL_loadbuffer(L, builder.data, builder.len, chunkname) != LUA_OK) {
        report_error(lua_tostring(L, -1), in_path, &lm);
        lua_pop(L, 1);
    } else if (lua_pcall(L, 0, 1, msgh_idx) != LUA_OK) {
        report_error(lua_tostring(L, -1), in_path, &lm);
        lua_pop(L, 1);
    } else if (lua_isstring(L, -1)) {
        /* leave the result string on the stack for the caller */
        success = true;
    } else {
        fprintf(stderr, "ERR: %s: template did not produce a string result\n", in_path);
        lua_pop(L, 1);
    }
    lua_remove(L, msgh_idx);

    str_deinit(&builder);
    str_deinit(&buffer);
    linemap_deinit(&lm);
    return success; /* stack top = result string iff true */
}

/* Parses+runs the template and writes the rendered result to out_path via C
 * stdio directly (never through Lua's io library). */
bool tmg_render(lua_State *L, const char *in_path, const char *out_path,
                 char sep, bool debug)
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

/* Parses+runs the template and hands the rendered string back to Lua
 * instead of writing it anywhere. Writing (if any) is then the calling
 * Lua code's responsibility, via normal Lua io. */
bool tmg_render_string(lua_State *L, const char *in_path, char sep, bool debug)
{
    /* leaves result string on stack on success, matching tmg_expand */
    return tmg_expand(L, in_path, sep, debug);
}

/* ---- Lua-facing glue ------------------------------------------------- */

static char resolve_sep(lua_State *L, int idx) {
    char sep = '%';
    if (!lua_isnoneornil(L, idx)) {
        const char *s = luaL_checkstring(L, idx);
        if (s[0]) sep = s[0];
    } else {
        lua_getglobal(L, "tmg_delim");
        if (lua_isstring(L, -1)) {
            const char *s = lua_tostring(L, -1);
            if (s && s[0]) sep = s[0];
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

static int l_tmg_render(lua_State *L) {
    const char *in_path  = luaL_checkstring(L, 1);
    const char *out_path = luaL_checkstring(L, 2);
    char sep = resolve_sep(L, 3);
    bool debug = resolve_debug(L);

    bool ok = tmg_render(L, in_path, out_path, sep, debug);
    lua_pushboolean(L, ok);
    return 1;
}

/* tmg.render_string(in_path [, sep]) -> string | nil
 * Same parsing/execution as tmg.render, but returns the rendered text
 * instead of writing it to a file. Writing it (or not) is on the Lua side
 * from here on: `local f = io.open(path, "w"); f:write(result); f:close()`. */
static int l_tmg_render_string(lua_State *L) {
    const char *in_path = luaL_checkstring(L, 1);
    char sep = resolve_sep(L, 2);
    bool debug = resolve_debug(L);

    if (tmg_render_string(L, in_path, sep, debug)) {
        return 1; /* result string already on top of the stack */
    }
    lua_pushnil(L);
    return 1;
}

static const luaL_Reg tmg_lib[] = {
    {"render", l_tmg_render},
    {"render_string", l_tmg_render_string},
    {NULL, NULL}
};

int luaopen_tmg(lua_State *L) {
    luaL_newlib(L, tmg_lib);
    return 1;
}

/* ---- entrypoint --------------------------------------------------------
 * tmg is a Lua runtime with templating built in: it does not render
 * anything by itself. Point it at a build script; the script calls
 * tmg.render(in, out) for every template it wants written. */
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

    luaL_requiref(L, "tmg", luaopen_tmg, 1); /* also sets global `tmg` */
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
