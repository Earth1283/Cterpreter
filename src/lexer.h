#ifndef CT_LEXER_H
#define CT_LEXER_H

#include "cterpreter.h"

typedef enum {
    TK_EOF = 256, TK_ERROR, TK_INTEGER, TK_REAL, TK_NAME,
    TK_INT, TK_DOUBLE, TK_VOID, TK_IF, TK_ELSE, TK_WHILE, TK_FOR,
    TK_RETURN, TK_BREAK, TK_CONTINUE, TK_EQ, TK_NE, TK_LE, TK_GE, TK_AND, TK_OR,
    TK_SHL, TK_SHR, TK_ADD_ASSIGN, TK_SUB_ASSIGN, TK_MUL_ASSIGN,
    TK_DIV_ASSIGN, TK_MOD_ASSIGN, TK_AND_ASSIGN, TK_OR_ASSIGN,
    TK_XOR_ASSIGN, TK_SHL_ASSIGN, TK_SHR_ASSIGN, TK_INCREMENT, TK_DECREMENT,
    TK_STRING, TK_CHAR, TK_DO, TK_SIZEOF, TK_ALIGNOF, TK_STATIC, TK_CONST,
    TK_SWITCH, TK_CASE, TK_DEFAULT, TK_GOTO, TK_TYPEDEF, TK_ENUM, TK_GENERIC,
    TK_STRUCT, TK_UNION, TK_ARROW, TK_ELLIPSIS,
    TK_SIGNED, TK_UNSIGNED, TK_SHORT, TK_LONG, TK_FLOAT, TK_BOOL,
    TK_VOLATILE, TK_RESTRICT, TK_EXTERN, TK_REGISTER, TK_INLINE, TK_AUTO, TK_STATIC_ASSERT
} TokenKind;

typedef struct {
    int kind;
    const char *start;
    size_t length, line, column;
    CtValue value;
} Token;

typedef struct {
    const char *cursor;
    size_t line, column;
    int incomplete;
    CtError error;
} Lexer;

void lexer_init(Lexer *lexer, const char *source);
Token lexer_next(Lexer *lexer);
char *lexer_decode(Token token, size_t *length, CtError *error);

#endif
