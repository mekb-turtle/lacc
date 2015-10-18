#ifndef _XOPEN_SOURCE
#  define _XOPEN_SOURCE 500 /* getopt */
#endif
#include "core/error.h"
#include "core/ir.h"
#include "core/string.h"
#include "core/symbol.h"
#include "frontend/input.h"
#include "frontend/preprocess.h"

#include <assert.h>
#include <stdio.h>
#include <unistd.h>

int VERBOSE = 0;

void help(const char *prog)
{
    fprintf(stderr, "Usage: %s [-S] [-E] [-v] [-I <path>] [-o <file>] <file>\n",
        prog);
}

extern struct decl *parse();
extern void fdotgen(FILE *, const struct decl *);
extern void assemble(FILE *, const struct decl *);

int main(int argc, char* argv[])
{
    char *input = NULL;
    FILE *output = stdout;
    int c;
    enum {
        OUT_DOT,
        OUT_ASSEMBLY,
        OUT_PREPROCESSED
    } output_mode = OUT_DOT;

    /* Handle command line parameters. */
    while ((c = getopt(argc, argv, "SEo:vI:")) != -1) {
        switch (c) {
        case 'S':
            output_mode = OUT_ASSEMBLY;
            break;
        case 'E':
            output_mode = OUT_PREPROCESSED;
            break;
        case 'o':
            output = fopen(optarg, "w");
            break;
        case 'v':
            VERBOSE = 1;
            break;
        case 'I':
            add_include_search_path(optarg);
            break;
        default:
            help(argv[0]);
            return 1;
        }
    }

    if (optind == argc - 1) {
        input = argv[optind];
    } else if (optind < argc - 1) {
        help(argv[0]);
        return 1;
    }

    /* Add default search paths last, with lowest priority. These are searched
     * after anything specified with -I. */
    add_include_search_path("/usr/include");
    add_include_search_path("/usr/local/include");

    init(input);
    register_builtin_definitions();

    if (output_mode == OUT_PREPROCESSED) {
        preprocess(output);
    } else {
        push_scope(&ns_ident);
        push_scope(&ns_tag);
        register_builtin_types(&ns_ident);

        while (1) {
            struct decl *fun = parse();
            if (errors || !fun) {
                if (errors) {
                    error("Aborting because of previous %s.",
                        (errors > 1) ? "errors" : "error");
                }
                break;
            }
            if (fun) {
                if (output_mode == OUT_ASSEMBLY) {
                    assemble(output, fun);
                } else {
                    assert(output_mode == OUT_DOT);
                    fdotgen(output, fun);
                }
                cfg_finalize(fun);
            }
        }

        if (output_mode == OUT_ASSEMBLY) {
            assemble_tentative_definitions(output);
            output_strings(output);
        }

        if (VERBOSE) {
            output_symbols(stdout, &ns_ident);
            output_symbols(stdout, &ns_tag);
        }

        pop_scope(&ns_tag);
        pop_scope(&ns_ident);
    }

    if (output != stdout)
        fclose(output);

    return errors;
}
