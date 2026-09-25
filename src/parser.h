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

/* How the optimizer found an N_NAME resolves: through the scope chain at run
 * time, through the one local declaration it lexically names, or to a global.
 * On a declaration, RESOLVE_LOCAL means only such names refer to it. */
enum { RESOLVE_DYNAMIC, RESOLVE_LOCAL, RESOLVE_GLOBAL };

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
    int cached, resolution;
    /* Declarations: 1 when no pointer can ever reach the object, so it needs no
     * address; -1 while the optimizer has seen its address taken. */
    int unaddressed;
    /* N_CALL: how many evaluations enclose it within its statement, itself included. */
    int nesting;
    /* How an operand is read: FETCH_EVALUATE, or in place as one of the others. */
    int fetch;
    /* N_BLOCK: declares nothing and cannot create temporaries, so it needs no scope. */
    int bare;
    /* The evaluator chosen for this node's shape before execution; NULL means the general one. */
    CtValue (*run)(CtInterpreter *interpreter, Node *node);
    /* The same for a statement, returning its control flow. */
    int (*perform)(CtInterpreter *interpreter, Node *node);
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
        /* N_INDEX and N_MEMBER: what the last operand type they saw leads to. */
        struct {
            CtType source; /* the operand type plus one; zero until one is seen */
            CtType target, decayed;
            int shape;
            size_t size, align;
        } access;
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
