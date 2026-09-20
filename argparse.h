#ifndef ARGPARSE_H
#define ARGPARSE_H

#include <stdbool.h>

struct popt {
    char *script;
    bool exit;
};

struct popt *parse_args(int argc, char **argv);
void cleanup_args(struct popt *opt);

#endif /* ARGPARSE_H */
