#include "argparse.h"

#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>

struct popt *parse_args(int argc, char **argv, int min) {
    struct popt *ret = malloc(sizeof(struct popt));

    if (argc < min) return NULL;
    bool captured = false;

    for (int i = 1; i < argc; i++) {
        if (captured) {
            captured = false;
            continue;
        }

        char *cur = argv[i];
        if (cur[0] == '-') {
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
                    ret->dict = argv[i+1];
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
                ret->dict = argv[i];
                break;
            default:
                break;
        }
    }
    ret->sep = '%';
    return ret;
}

void cleanup_args(struct popt *opt) {
    free(opt);
}
