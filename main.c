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

    str_t out_builder = str_init(in_size * 2);
    str_t buffer = str_init(100);

    int c;
    bool skip = false;
    /* do the work */
    while ((c = fgetc(in)) != EOF) {
        if (c == opt->sep) {
            skip = !skip;
            if (buffer.len > 0) {
                if (luaL_dostring(L, buffer.data) != LUA_OK) {
                    fprintf(stderr, "ERR: Lua error: %s\n", lua_tostring(L, -1));
                    lua_pop(L, 1);
                } else {
                    if (lua_gettop(L) >= 1) {
                        if (lua_isstring(L, -1)) {
                            const char *result = lua_tostring(L, -1);
                            str_push(&out_builder, result);
                        }
                    }
                    lua_settop(L, 0);
                }
                str_clear(&buffer);
            }
            continue;
        }
        if (!skip && c == '\\') {
            str_push_chr(&out_builder, fgetc(in));
            continue;
        }
        else if (skip && c == '\\') {
            str_push_chr(&buffer, fgetc(in));
            continue;
        }
        else if (!skip) str_push_chr(&out_builder, c);
        else {
            if (c == '$') str_push(&buffer, "return ");
            else str_push_chr(&buffer, c);
        }
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
    fprintf(out, "%s", out_builder.data);

    /* closing */
    lua_close(L);
    cleanup_args(opt);
    str_deinit(&out_builder);
    str_deinit(&buffer);
    fclose(in);
    fclose(out);
    return EXIT_SUCCESS;
}
