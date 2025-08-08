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

static int print_capture(lua_State *L) {
    // Get the number of arguments passed to the function `r`.
    int n = lua_gettop(L);
    luaL_Buffer b;

    // Initialize a buffer to build the string.
    luaL_buffinit(L, &b);

    // Loop through all the arguments.
    for (int i = 1; i <= n; i++) {
        // This is the first fix:
        // luaL_tolstring converts the argument at index `i` to a string
        // and pushes that new string to the TOP of the stack. This is safer
        // than luaL_checkstring as it can also convert numbers.
        luaL_tolstring(L, i, NULL);

        // luaL_addvalue takes the string from the TOP of the stack and adds
        // it to the buffer. This now works because luaL_tolstring pushed it there.
        luaL_addvalue(&b);
    }

    // Push the final concatenated string from the buffer onto the stack.
    luaL_pushresult(&b);

    // This is the second, crucial fix:
    // Return 1 to tell the Lua interpreter that this C function is returning
    // ONE value (the string we just pushed).
    return 1;
}

int main(int argc, char **argv)
{
    const int MIN_ARGS = 3;
    struct popt *opt = parse_args(argc, argv, MIN_ARGS);
    if (!opt) return EXIT_FAILURE;

    lua_State *L = luaL_newstate();
    if (L == NULL) {
        fprintf(stderr, "ERR: Failed to create Lua state.\n");
        return EXIT_FAILURE;
    }
    luaL_openlibs(L);
    lua_pushcfunction(L, print_capture);
    lua_setglobal(L, "r");


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
    
    int old;
    int c;
    bool skip = false;
    while ((c = fgetc(in)) != EOF) {
        if (c == opt->sep && old != '\\') {
            skip = !skip;
            if (buffer.len > 0) {
                if (luaL_dostring(L, buffer.data) != LUA_OK) {
                    fprintf(stderr, "ERR: Lua error: %s\n", lua_tostring(L, -1));
                    lua_pop(L, 1);
                } else {
                    const char *result = lua_tostring(L, -1);
                    if (result) {
                        str_push(&out_builder, result);
                        lua_pop(L, 1);
                    }
                }
                str_clear(&buffer);
            }
            old = c;
            continue;
        }
        if (!skip) str_push_chr(&out_builder, c);
        else       str_push_chr(&buffer, c);
        old = c;
    }

    printf("%s\n", out_builder.data);

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

    lua_close(L);
    cleanup_args(opt);
    str_deinit(&out_builder);
    str_deinit(&buffer);
    fclose(in);
    fclose(out);
    // fclose(dict);
    return EXIT_SUCCESS;
}
