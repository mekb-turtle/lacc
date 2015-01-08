#include "error.h"
#include "symbol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the actual symbol table */
static symbol_t **symtab;
static int symtab_size;
static int symtab_capacity;

/* stack structure to keep track of lexical scope */
static struct lexical_scope {
    const symbol_t **symlist; /* points to symtab */
    size_t size;
    size_t cap;
} *scopes = NULL;

/* Nesting level of lexical scope, incremented for each { } block.
 * 0: Global scope
 * 1: Function arguments
 * n: Automatic variables
 */
static int depth = -1;
static int scope_cap;

/* Track offset of local variables on stack. */
int var_stack_offset;
int var_param_number;

void push_scope()
{
    depth++;
    if (depth == scope_cap) {
        scope_cap += 16;
        scopes = realloc(scopes, sizeof(struct lexical_scope) * scope_cap);
        memset(&scopes[depth], 0x0, sizeof(struct lexical_scope) * 16);
    }
    /* Reset for function params and body. */
    if (depth == 1) {
        var_stack_offset = 8; /* stack pointer */
        var_param_number = 1;
    }
    if (depth == 2) {
        var_stack_offset = 0;
    }
}

void pop_scope()
{
    if (depth >= 0) {
        free(scopes[depth].symlist);
        memset(&scopes[depth], 0x0, sizeof(struct lexical_scope));
        depth--;
    }
    if (depth == -1) {
        free(scopes);
        scopes = NULL;
    }
}

/* Create and add symbol to symbol table, but not to any scope. Immediate values
 * can have NULL as name, and offset 0. */
static symbol_t *
sym_init(const char *name, const typetree_t *type, int param, int offset)
{
    if (symtab_size == symtab_capacity) {
        symtab_capacity += 64;
        symtab = realloc(symtab, sizeof(symbol_t*) * symtab_capacity);
    }
    /* symbol address needs to be stable, so never realloc this */
    symtab[symtab_size] = calloc(1, sizeof(symbol_t));
    symtab[symtab_size]->name = (name == NULL) ? name : strdup(name);
    symtab[symtab_size]->type = type;
    symtab[symtab_size]->depth = depth;
    symtab[symtab_size]->param_n = param;
    symtab[symtab_size]->stack_offset = offset;
    return symtab[symtab_size++];
}

/* Helper to create symbols without name */
static symbol_t *
sym_init_temp(const typetree_t *type, int offset)
{
    static int tmpn;

    char tmpname[16];
    do {
        snprintf(tmpname, 12, "t%d", tmpn++);
    } while (sym_lookup(tmpname) != NULL);

    return sym_init(tmpname, type, 0, offset);
}

/* Add symbol to current scope, making it possible to look up. Name must be non-
 * NULL, i.e. immediate values do not belong to any scope. */
static void
sym_register(const symbol_t *symbol)
{
    struct lexical_scope *scope = &scopes[depth];
    if (!symbol->name) {
        error("Registering symbol with missing name.");
    }
    if (scope->size == scope->cap) {
        scope->cap += 16;
        scope->symlist = realloc(scope->symlist, sizeof(symbol_t*) * scope->cap);
    }
    scope->symlist[scope->size] = symbol;
    scope->size++;
}

/* Retrieve a symbol based on identifier name, or NULL of not registered or
 * visible from current scope. */
const symbol_t *
sym_lookup(const char *name)
{
    const symbol_t *sym;
    int i, d;
    for (d = depth; d >= 0; --d) {
        for (i = 0; i < scopes[d].size; ++i) {
            sym = scopes[d].symlist[i];
            if (!strcmp(name, sym->name)) {
                return sym;
            }
        }
    }
    return NULL;
}

/* Add symbol to current scope. */
const symbol_t *
sym_add(const char *name, const typetree_t *type)
{
    const symbol_t *symbol = sym_lookup(name);
    int param = 0;
    int offset = 0;

    if (symbol != NULL && symbol->depth == depth) {
        error("Duplicate definition of symbol '%s'", name);
        exit(0);
    }
    /* x86_64 specific, wrong if params cannot fit in registers. */
    if (depth == 1) {
        param = var_param_number++;
        var_stack_offset += type_size(type);
        if (param > 6)
            offset = var_stack_offset;
    } else if (depth > 1) {
        var_stack_offset -= type_size(type);
        offset = var_stack_offset;
    }
    symbol = (const symbol_t *) sym_init(name, type, param, offset);
    sym_register(symbol);
    return symbol;
}

/* Add temporary (autogenerated name) symbol to current scope. */
const symbol_t *
sym_temp(const typetree_t *type)
{
    symbol_t *symbol;
    int offset = 0;

    if (depth == 1) {
        var_stack_offset += type_size(type);
        offset = var_stack_offset;
    } else if (depth > 1) {
        var_stack_offset -= type_size(type);
        offset = var_stack_offset;
    }

    symbol = sym_init_temp(type, offset);
    sym_register(symbol);
    return symbol;
}

void
dump_symtab()
{
    int i;
    char *tstr;
    for (i = 0; i < symtab_size; ++i) {
        printf("%*s", symtab[i]->depth * 2, "");
        printf("%s :: ", symtab[i]->name);
        tstr = typetostr(symtab[i]->type);
        printf("%s", tstr);
        free(tstr);
        if (symtab[i]->value) {
            switch (symtab[i]->type->type) {
                case INT64_T:
                    printf(" = %d", (int) symtab[i]->value->v_long);
                    break;
                case ARRAY:
                case POINTER:
                    if (symtab[i]->type->next 
                        && symtab[i]->type->next->type == CHAR_T) {
                        printf(" = \"%s\"", symtab[i]->value->v_string);
                        break;
                    }
                default:
                    printf(" = immediate");
                    break;
            }
        }
        printf(", size=%d", symtab[i]->type->size);
        if (symtab[i]->type->length)
            printf(", length=%d", symtab[i]->type->length);
        if (symtab[i]->param_n) {
            if (symtab[i]->stack_offset > 0) {
                printf(" (param: %d, offset: %d)", symtab[i]->param_n, symtab[i]->stack_offset);
            } else {
                printf(" (param: %d)", symtab[i]->param_n);
            }
        }
        if (symtab[i]->stack_offset < 0) {
            printf(" (auto: %d)", symtab[i]->stack_offset);
        }
        printf("\n");
    }
}
