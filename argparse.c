#include "argparse.h"
#include "version.h"

#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>

static inline void print_version(const char *name) {
    printf("%s %d.%d.%d\n", name, VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH);
}

static inline void print_help(const char *name) {
    printf("%s -i [input] -o [output] <flags>\n", name);
    printf("<flags> :\n");
    printf("  -i [f] : input file\n");
    printf("  -o [f] : output file\n");
    printf("  -d [c] : set the delimiter char (default `%%`).\n");
    printf("  -p     : print the generated code to stdout.\n");
}

struct popt *parse_args(int argc, char **argv, int min) {
    struct popt *ret = malloc(sizeof(struct popt));
    ret->sep = '%';
    ret->print_code = false;
    ret->exit = false;

    // if (argc < min) return NULL;
    bool captured = false;

    for (int i = 1; i < argc; i++) {
        if (captured) {
            captured = false;
            continue;
        }

        char *cur = argv[i];
        if (cur[0] == '-') {
            if (cur[1] == 'p') {
                ret->print_code = true;
                continue;
            }
            if (cur[1] == 'h' || cur[i] == 'v') {
                if (cur[1] == 'v') print_version(argv[0]);
                if (cur[1] == 'h') print_help(argv[0]);
                ret->exit = true;
                break;
            }
            if (i + 1 >= argc) {
                fprintf(stderr, "ERR: `%s` Flag need something after them.\n", cur);
                return NULL;
            }

            switch (cur[1]) {
                case 'i':
                    ret->in = argv[i+1];
                    captured = true;
                    break;
                case 'o':
                    ret->out = argv[i+1];
                    captured = true;
                    break;
                case 'd':
                    ret->sep = *argv[i+1];
                    captured = true;
                    break;
                default:
                    fprintf(stderr, "ERR: Invalid flag `%c`.\n", argv[i][1]);
                    return NULL;
            }
            continue;
        }
        switch (i) {
            case 1:
                ret->in = argv[i];
                break;
            case 2:
                ret->out = argv[i];
                break;
            case 3:
                ret->sep = *argv[i];
                break;
            default:
                break;
        }
    }
    return ret;
}

void cleanup_args(struct popt *opt) {
    free(opt);
}
