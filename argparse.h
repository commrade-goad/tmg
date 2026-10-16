#ifndef ARGPARSE_H
#define ARGPARSE_H

#include <stdbool.h>

struct popt {
    char *script;
    int script_idx;   /* index of the script in argv; everything after it
                         belongs to the script (see arg / ... in Lua) */
    bool exit;
};

struct popt *parse_args(int argc, char **argv);
void cleanup_args(struct popt *opt);

#endif /* ARGPARSE_H */
