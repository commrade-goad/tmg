#include "argparse.h"
#include "version.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

static inline void print_version(const char *name) {
    printf("%s %d.%d.%d\n", name, VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH);
}

static inline void print_help(const char *name) {
    printf("%s <script.lua>\n", name);
    printf("  -h : print this help\n");
    printf("  -v : print version\n\n");
    printf("tmg is a Lua runtime with templating built in. Write a build\n");
    printf("script that calls tmg.render(input, output [, delim]) to render\n");
    printf("`.tmg` templates. Set a global G_delim before calling render to\n");
    printf("change the default delimiter (`%%`), or G_debug = true to print\n");
    printf("generated chunks to stderr.\n");
}

struct popt *parse_args(int argc, char **argv) {
    struct popt *ret = malloc(sizeof(struct popt));
    ret->script = NULL;
    ret->exit = false;

    for (int i = 1; i < argc; i++) {
        char *cur = argv[i];
        if (strcmp(cur, "-h") == 0) {
            print_help(argv[0]);
            ret->exit = true;
            return ret;
        }
        if (strcmp(cur, "-v") == 0) {
            print_version(argv[0]);
            ret->exit = true;
            return ret;
        }
        if (!ret->script) {
            ret->script = cur;
        }
    }
    return ret;
}

void cleanup_args(struct popt *opt) {
    free(opt);
}
