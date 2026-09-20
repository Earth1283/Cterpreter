#ifndef CT_PARSER_H
#define CT_PARSER_H

#include "lexer.h"

typedef enum {
    N_VALUE, N_NAME, N_UNARY, N_POSTFIX, N_BINARY, N_CONDITIONAL, N_CALL,
    N_DECLARATION, N_EXPRESSION, N_BLOCK, N_IF, N_WHILE, N_FOR,
    N_RETURN, N_BREAK, N_CONTINUE, N_FUNCTION, N_EMPTY, N_STRING, N_INDEX,
    N_CAST, N_SIZEOF, N_ALIGNOF, N_GROUP, N_INITIALIZER, N_DO,
    N_SWITCH, N_CASE, N_GOTO, N_LABEL, N_TYPEDEF, N_ENUMERATOR, N_GENERIC, N_ASSOCIATION
} NodeKind;

typedef struct Node Node;
struct Node {
    NodeKind kind;
    Token token;
    CtType type;
    char *text;
    size_t text_length;
    int is_array, is_static, is_const, terminated, enum_tag;
    uint64_t address;
    Node *left, *right, *third, *fourth, *next;
    Node *allocated_next;
};

typedef struct Unit Unit;
struct Unit {
    char *source;
    Node *statements;
    Node *allocations;
    Unit *next;
    int has_functions;
};

CtStatus parse(const char *source, Unit **unit, CtError *error);
CtStatus parse_with_context(const char *source, const Unit *previous, Unit **unit, CtError *error);
void unit_destroy(Unit *unit);
void dump_ast(const Unit *unit, FILE *output);
int is_assignment(int kind);

#endif
