#if _XOPEN_SOURCE < 500
#  undef _XOPEN_SOURCE
#  define _XOPEN_SOURCE 500 /* strdup */
#endif
#include "error.h"
#include "symbol.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void push_scope(struct namespace *ns) {
    assert(ns);
    ns->current_depth++;
    if (!ns->scope) {
        ns->current_depth = 0;
    }
    ns->scope = realloc(ns->scope, sizeof(*ns->scope) * (ns->current_depth+1));
    memset(&ns->scope[ns->current_depth], 0x0, sizeof(struct scope));
}

void pop_scope(struct namespace *ns) {
    if (ns->current_depth >= 0) {
        if (ns->scope[ns->current_depth].idx) {
            free(ns->scope[ns->current_depth].idx);    
        }
        memset(&ns->scope[ns->current_depth], 0x0, sizeof(struct scope));
        ns->current_depth--;
    }
    if (ns->current_depth == -1) {
        if (ns->scope) {
            free(ns->scope);
        }
        ns->scope = NULL;
    }
}

const char *sym_name(const struct symbol *sym)
{
    if (sym->n) {
        static char name[128];
        snprintf(name, 127, "%s.%d", sym->name, sym->n);
        return name;
    }

    return sym->name;
}

/* Create and add symbol to symbol table, but not to any scope. Symbol address
 * needs to be stable, so they are stored as a realloc'able list of pointers.
 */
static int create_symbol(struct namespace *ns, struct symbol sym)
{
    sym.depth = ns->current_depth;
    if (ns->size == ns->cap) {
        ns->cap += 64;
        ns->symbol = realloc(ns->symbol, sizeof(struct symbol*) * ns->cap);
    }

    ns->symbol[ns->size] = calloc(1, sizeof(struct symbol));
    *(ns->symbol[ns->size]) = sym;

    return ns->size++;
}

/* Create a temporary identifier name. Use a fixed prefix '.' to all temporary 
 * variables, which will never collide with real symbols.
 */
static char *unique_identifier_name(void)
{
    static int n;
    char *c = malloc(16);
    snprintf(c, 16, ".t%d", n++);
    return c;
}

/* Add symbol to current scope, making it possible to look up. Name must be non-
 * NULL, i.e. immediate values do not belong to any scope.
 */
static void register_in_scope(struct namespace *ns, int i)
{
    struct scope *scope;

    assert(i < ns->size);
    scope = &ns->scope[ns->current_depth];
    if (scope->size == scope->cap) {
        scope->cap += 16;
        scope->idx = realloc(scope->idx, scope->cap * sizeof(int *));
    }
    scope->idx[scope->size] = i;
    scope->size++;
}

static void print_symbol(struct symbol *sym)
{
    printf("\t[type: %s",
        sym->symtype == SYM_DEFINITION ? "definition" :
        sym->symtype == SYM_TENTATIVE ? "tentative" :
        sym->symtype == SYM_DECLARATION ? "declaration" :
        sym->symtype == SYM_TYPEDEF ? "typedef" : "enum"
    );
    printf(", link: %s]\n",
        sym->linkage == LINK_INTERN ? "intern" :
        sym->linkage == LINK_EXTERN ? "extern" : "none"
    );
    printf("\t%s", sym->name);
    if (sym->n) {
        printf(".%d", sym->n);
    }
    printf(" :: %s\n", typetostr(sym->type));
}

/* Retrieve a symbol based on identifier name, or NULL of not registered or
 * visible from current scope.
 */
struct symbol *sym_lookup(struct namespace *ns, const char *name)
{
    int i, d;

    assert(ns);
    for (d = ns->current_depth; d >= 0; --d) {
        for (i = 0; i < ns->scope[d].size; ++i) {
            int idx = ns->scope[d].idx[i];
            if (!strcmp(name, ns->symbol[idx]->name)) {
                return ns->symbol[idx];
            }
        }
    }
    return NULL;
}

/* Add symbol to current scope, or resolve to or complete existing symbols when
 * they occur repeatedly.
 */
struct symbol *sym_add(struct namespace *ns, struct symbol sym)
{
    int idx;
    struct symbol *s;
    static int svc;
    extern int VERBOSE;

    if ((s = sym_lookup(ns, sym.name))) {
        if (
            sym.linkage == LINK_EXTERN && sym.symtype == SYM_DECLARATION &&
            (s->symtype == SYM_TENTATIVE || s->symtype == SYM_DEFINITION)
        ) {
            if (!s->type->size) {
                s->type = type_complete(s->type, sym.type);
            }
            return s;
        }
        if (s->depth == ns->current_depth && ns->current_depth == 0) {
            if (s->linkage == sym.linkage && (
                (s->symtype == SYM_TENTATIVE && sym.symtype == SYM_DEFINITION) || 
                (s->symtype == SYM_DEFINITION && sym.symtype == SYM_TENTATIVE))
            ) {
                if (!s->type->size) {
                    s->type = type_complete(s->type, sym.type);
                }
                s->symtype = SYM_DEFINITION;
            } else if (
                s->linkage == sym.linkage &&
                s->symtype == SYM_DECLARATION &&
                sym.symtype == SYM_TENTATIVE
            ) {
                if (!s->type->size) {
                    s->type = type_complete(s->type, sym.type);
                }
                s->symtype = SYM_TENTATIVE;
            } else if (
                s->symtype != sym.symtype || s->linkage != sym.linkage
            ) {
                error("Declaration of '%s' does not match prior declaration.",
                    sym.name);
                exit(1);
            } else {
                if (!s->type->size) {
                    s->type = type_complete(s->type, sym.type);
                }
            }
            return s;
        } else if (s->depth == ns->current_depth && ns->current_depth) {
            error("Duplicate definition of symbol '%s'", sym.name);
            exit(1);
        }
    }

    /* Might not be needed. */
    sym.name = strdup(sym.name);

    /* Scoped static variable must get unique name in order to not collide with
     * other external declarations. */
    if (sym.linkage == LINK_INTERN && ns->current_depth) {
        sym.n = ++svc;
    }

    idx = create_symbol(ns, sym);
    register_in_scope(ns, idx);
    s = ns->symbol[idx];
    if (VERBOSE) {
        print_symbol(s);
    }

    return s;
}

/* Create a symbol with the provided type and add it to current scope. Used to
 * hold temporary values in expression evaluation.
 */
struct symbol *sym_temp(struct namespace *ns, const struct typetree *type)
{
    int idx;
    struct symbol sym = {0};
    sym.name = unique_identifier_name();
    sym.type = type;
    idx = create_symbol(ns, sym);
    register_in_scope(ns, idx);
    return ns->symbol[idx];
}

/* Register compiler internal builtin symbols, that are assumed to exists by
 * standard library headers.
 */
void register_builtin_types(struct namespace *ns)
{
    /* Define va_list, as described in System V ABI. */
    struct symbol sym = {"__builtin_va_list", NULL, SYM_TYPEDEF};
    struct typetree *type = type_init_object();
    type_add_member(type, type_init_unsigned(4), "gp_offset");
    type_add_member(type, type_init_unsigned(4), "fp_offset");
    type_add_member(type, type_init_pointer(type_init_void()),
        "overflow_arg_area");
    type_add_member(type, type_init_pointer(type_init_void()), "reg_save_area");
    type_align_struct_members(type);
    sym.type = type_init_array(type, 1);
    sym_add(ns, sym);

    /* Register symbols with dummy types just to reserve them, and make them
     * resolve during parsing. */
    sym.name = "__builtin_va_start";
    sym.type = type_init_void();
    sym.symtype = SYM_DECLARATION;
    sym_add(ns, sym);

    sym.name = "__builtin_va_arg";
    sym_add(ns, sym);
}

static int sym_asm_alignment(const struct symbol *sym)
{
    int w = sym->type->size;
    if (w >= 16) return 16;
    if (w >= 8) return 8;
    return 4;
}

void assemble_tentative_definitions(FILE *stream)
{
    extern struct namespace ns_ident;

    int i;
    struct symbol *sym;

    for (i = 0; i < ns_ident.size; ++i) {
        sym = ns_ident.symbol[i];
        if (sym->symtype == SYM_TENTATIVE && is_object(sym->type)) {
            if (sym->linkage == LINK_INTERN) {
                fprintf(stream, "\t.local %s\n", sym_name(sym));
            }
            fprintf(stream, "\t.comm %s, %d, %d\n",
                sym_name(sym), sym->type->size, sym_asm_alignment(sym));
        }
    }
}

void output_symbols(FILE *stream, struct namespace *ns)
{
    int i;
    char *tstr;

    if (ns->size) {
        fprintf(stream, "namespace %s:\n", ns->name);
    }
    for (i = 0; i < ns->size; ++i) {
        enum symtype st = ns->symbol[i]->symtype;

        fprintf(stream, "%*s", ns->symbol[i]->depth * 2, "");
        if (ns->symbol[i]->linkage != LINK_NONE) {
            fprintf(stream, "%s ",
                (ns->symbol[i]->linkage == LINK_INTERN) ? "static" : "global");
        }
        fprintf(stream, "%s ",
            (st == SYM_TENTATIVE) ? "tentative" : 
            (st == SYM_DEFINITION) ? "definition" :
            (st == SYM_DECLARATION) ? "declaration" :
            (st == SYM_TYPEDEF) ? "typedef" : "enum");

        fprintf(stream, "%s :: ", ns->symbol[i]->name);
        tstr = typetostr(ns->symbol[i]->type);
        fprintf(stream, "%s", tstr);
        free(tstr);

        fprintf(stream, ", size=%d", ns->symbol[i]->type->size);
        if (ns->symbol[i]->stack_offset) {
            fprintf(stream, " (stack_offset: %d)", ns->symbol[i]->stack_offset);
        }
        if (ns->symbol[i]->symtype == SYM_ENUM_VALUE) {
            fprintf(stream, ", value=%d", ns->symbol[i]->enum_value);
        }
        fprintf(stream, "\n");
    }
}
