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

#define MIN_ARGS 3

bool only_whitespace(char *str) {
    char *ptr = str;
    while (ptr && *ptr != '\0') {
        if (!isspace(*ptr++)) return false;
    }
    return true;
}

int main(int argc, char **argv)
{
    /* init */
    struct popt *opt = parse_args(argc, argv, MIN_ARGS);
    if (!opt) return EXIT_FAILURE;

    lua_State *L = luaL_newstate();
    if (L == NULL) {
        fprintf(stderr, "ERR: Failed to create Lua state.\n");
        return EXIT_FAILURE;
    }
    luaL_openlibs(L);

    /* read input file */
    FILE *in = fopen(opt->in, "r");
    if (!in) {
        fprintf(stderr,
                "ERR: Failed to read the input file `%s`: %s\n",
                opt->in, strerror(errno)
                );
        lua_close(L);
        return EXIT_FAILURE;
    }
    fseek(in, 0, SEEK_END);
    long in_size = ftell(in);
    rewind(in);

    str_t builder = str_init(in_size * 2);
    str_t buffer = str_init(100);
    str_t final =  str_init(in_size * 2);

    /* start parsing */
    str_push(&builder, "local out = {}\n");

    bool code = false;
    bool inline_code = false;
    int c;
    /* do the work */
    while ((c = fgetc(in)) != EOF) {
        if (c == opt->sep) {
            if (inline_code) str_push(&builder, ")");
            str_push_chr(&builder, '\n');

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
                str_clear(&buffer);
            }
            continue;
        }
        if (code) {
            if (c == '\\') {
                int next = fgetc(in);
                if (!isspace(next))
                    str_push_chr(&builder, next);
                continue;
            }
            if (c == '$') {
                inline_code = true;
                c = fgetc(in);
                str_push(&builder, "out[#out+1] = ");
                str_push(&builder, "tostring(");
            }
            str_push_chr(&builder, c);
            continue;
        } else {
            if (c == '\\') {
                int next = fgetc(in);
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

    str_push(&builder, "return table.concat(out)\n");
    /* end parsing */

    if (opt->print_code) printf("%s\n", builder.data);

    if (luaL_dostring(L, builder.data) != LUA_OK) {
        fprintf(stderr, "ERR: Lua error: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
    } else {
        if (lua_gettop(L) >= 1) {
            if (lua_isstring(L, -1)) {
                const char *result = lua_tostring(L, -1);
                str_push(&final, result);
            }
        }
        lua_settop(L, 0);
    }

    /* write to a file */
    FILE *out = fopen(opt->out, "w");
    if (!out) {
        fprintf(stderr,
                "ERR: Failed to create the output file `%s`: %s\n",
                opt->out, strerror(errno)
                );
        lua_close(L);
        return EXIT_FAILURE;
    }
    fprintf(out, "%s", final.data);

    /* closing */
    lua_close(L);
    cleanup_args(opt);
    str_deinit(&builder);
    str_deinit(&buffer);
    str_deinit(&final);
    fclose(in);
    fclose(out);
    return EXIT_SUCCESS;
}
