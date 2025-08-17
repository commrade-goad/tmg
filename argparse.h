#ifndef ARGPARSE_H
#define ARGPARSE_H

#include <stdbool.h>

struct popt {
    char *in, *out;
    char sep;
    bool print_code;
    bool exit;
};

struct popt *parse_args(int argc, char **argv, int min);
void cleanup_args(struct popt *opt);

#endif /* ARGPARSE_H */
