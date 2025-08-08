#ifndef ARGPARSE_H
#define ARGPARSE_H

struct popt {
    char *in, *out;
    char sep;
};

struct popt *parse_args(int argc, char **argv, int min);
void cleanup_args(struct popt *opt);

#endif /* ARGPARSE_H */
