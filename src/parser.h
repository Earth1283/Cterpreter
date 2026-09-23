#ifndef CT_PARSER_H
#define CT_PARSER_H

#include "lexer.h"
#include "types.h"

typedef enum {
    N_VALUE, N_NAME, N_UNARY, N_POSTFIX, N_BINARY, N_CONDITIONAL, N_CALL,
    N_DECLARATION, N_EXPRESSION, N_BLOCK, N_IF, N_WHILE, N_FOR,
    N_RETURN, N_BREAK, N_CONTINUE, N_FUNCTION, N_EMPTY, N_STRING, N_INDEX,
    N_CAST, N_SIZEOF, N_ALIGNOF, N_GROUP, N_INITIALIZER, N_DO,
    N_SWITCH, N_CASE, N_GOTO, N_LABEL, N_TYPEDEF, N_ENUMERATOR, N_GENERIC, N_ASSOCIATION,
    N_MEMBER, N_COMPOUND, N_DESIGNATED, N_DESIGNATOR,
    N_VA_START, N_VA_ARG, N_VA_END, N_VA_COPY, N_COMMA
} NodeKind;

/* Tags share a namespace per kind; TAG_NAME covers typedef names. */
typedef enum { TAG_NAME, TAG_ENUM, TAG_STRUCT, TAG_UNION } TagKind;

typedef struct Node Node;
struct Symbol;
struct Node {
    NodeKind kind;
    Token token;
    CtType type;
    char *text;
    size_t text_length;
    int is_static, is_extern, is_const, terminated, tag_kind, local, through_pointer, variadic, builtin_id;
    uint64_t address;
    Node *left, *right, *third, *fourth, *next;
    Node *allocated_next;
    Node *alias_next;
    uint64_t hash;
    int cached;
    /* Runtime caches; which member is live depends on the node kind. */
    union {
        /* N_NAME: the last resolution, reused while the innermost scope instance
         * and its symbol count are unchanged (only it can gain declarations). */
        struct {
            struct Symbol *symbol;
            Node *declaration;
            uint64_t scope, symbol_scope;
            size_t symbols;
        } use;
        /* Declarations: the innermost live symbol declared here and its scope. */
        struct {
            struct Symbol *symbol;
            uint64_t scope;
        } binding;
        struct {
            Node *head, *label;
        } jump;             /* N_GOTO: the label found in the statement list at head */
        CtValue constant;   /* N_CASE: the label value, once N_SWITCH is cached */
        CtType static_type; /* N_CONDITIONAL, once cached */
    } cache;
};

typedef struct Unit Unit;
typedef struct NodeBlock NodeBlock;
struct Unit {
    char *source;
    Node *statements;
    Node *allocations;
    Node *aliases;
    NodeBlock *node_blocks;
    Unit *next;
    int has_functions;
};

CtStatus parse(const char *source, Unit **unit, CtError *error);
CtStatus parse_with_context(const char *source, const Unit *previous, Unit **unit, CtError *error);
void unit_destroy(Unit *unit);
void dump_ast(const Unit *unit, FILE *output);
static inline int is_assignment(int kind) {
    return kind == '=' || (kind >= TK_ADD_ASSIGN && kind <= TK_SHR_ASSIGN);
}

#endif
