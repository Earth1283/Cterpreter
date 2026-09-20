#include "lexer.h"
#include "types.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void lexer_init(Lexer *lexer, const char *source) {
    *lexer = (Lexer){.cursor = source, .line = 1, .column = 1};
}

static void advance(Lexer *lexer) {
    if (*lexer->cursor == '\n') { ++lexer->line; lexer->column = 1; }
    else { ++lexer->column; }
    ++lexer->cursor;
}

/* Width suffixes are accepted and ignored: every integer is an int here. */
static int suffix_ends(const char *cursor, const char *end, const char *allowed) {
    for (; cursor != end; ++cursor) if (!strchr(allowed, *cursor)) return 0;
    return 1;
}

/* C17 6.4.4.1: the literal takes the first type in its list that can hold it. */
static CtType literal_type(unsigned long long value, int is_unsigned, int long_count, int is_octal_or_hex) {
    static const CtType signed_first[] = {CT_INT, CT_LONG, CT_LLONG};
    static const CtType unsigned_first[] = {CT_UINT, CT_ULONG, CT_ULLONG};
    for (int width = long_count > 2 ? 2 : long_count; width < 3; ++width) {
        if (!is_unsigned) {
            CtType candidate = signed_first[width];
            if (value <= (unsigned long long)type_maximum(candidate)) return candidate;
        }
        if (is_unsigned || is_octal_or_hex) {
            CtType candidate = unsigned_first[width];
            if (value <= type_mask(candidate)) return candidate;
        }
    }
    return CT_VOID;
}

static Token bad(Lexer *lexer, Token token, const char *message) {
    token.kind = TK_ERROR;
    lexer->error.line = token.line;
    lexer->error.column = token.column;
    (void)snprintf(lexer->error.message, sizeof lexer->error.message, "%s", message);
    return token;
}

Token lexer_next(Lexer *lexer) {
    for (;;) {
        while (isspace((unsigned char)*lexer->cursor)) advance(lexer);
        if (lexer->cursor[0] == '/' && lexer->cursor[1] == '/') {
            while (*lexer->cursor && *lexer->cursor != '\n') advance(lexer);
        } else if (lexer->cursor[0] == '/' && lexer->cursor[1] == '*') {
            Token opening = {.line = lexer->line, .column = lexer->column};
            advance(lexer); advance(lexer);
            while (*lexer->cursor && !(lexer->cursor[0] == '*' && lexer->cursor[1] == '/'))
                advance(lexer);
            if (!*lexer->cursor) {
                lexer->incomplete = 1;
                return bad(lexer, opening, "unterminated block comment");
            }
            advance(lexer); advance(lexer);
        } else break;
    }
    Token token = {.start = lexer->cursor, .line = lexer->line, .column = lexer->column};
    unsigned char first = (unsigned char)*lexer->cursor;
    if (!first) { token.kind = TK_EOF; return token; }
    if (first == '"' || first == '\'') {
        advance(lexer);
        while (*lexer->cursor && (unsigned char)*lexer->cursor != first) {
            if (*lexer->cursor == '\n') return bad(lexer, token, "newline in a quoted literal");
            if (*lexer->cursor == '\\') {
                advance(lexer);
                if (!*lexer->cursor) break;
            }
            advance(lexer);
        }
        if (!*lexer->cursor) {
            lexer->incomplete = 1;
            return bad(lexer, token, "unterminated quoted literal");
        }
        advance(lexer);
        token.length = (size_t)(lexer->cursor - token.start);
        token.kind = TK_STRING;
        size_t length;
        char *decoded = lexer_decode(token, &length, &lexer->error);
        if (!decoded) { token.kind = TK_ERROR; return token; }
        if (first == '\'') {
            if (length != 1) { free(decoded); return bad(lexer, token, "character literal must contain one byte"); }
            token.kind = TK_INTEGER;
            token.value = (CtValue){.type = CT_INT, .as.integer = (unsigned char)decoded[0]};
        }
        free(decoded);
        return token;
    }
    if (isalpha(first) || first == '_') {
        do { advance(lexer); } while (isalnum((unsigned char)*lexer->cursor) || *lexer->cursor == '_');
        token.length = (size_t)(lexer->cursor - token.start);
        token.kind = TK_NAME;
        static const struct { const char *name; int kind; } keywords[] = {
            {"int", TK_INT}, {"double", TK_DOUBLE}, {"void", TK_VOID},
            {"if", TK_IF}, {"else", TK_ELSE}, {"while", TK_WHILE},
            {"for", TK_FOR}, {"return", TK_RETURN}, {"break", TK_BREAK},
            {"continue", TK_CONTINUE}, {"char", TK_CHAR}, {"do", TK_DO},
            {"sizeof", TK_SIZEOF}, {"_Alignof", TK_ALIGNOF}, {"static", TK_STATIC},
            {"const", TK_CONST}, {"switch", TK_SWITCH}, {"case", TK_CASE},
            {"default", TK_DEFAULT}, {"goto", TK_GOTO}, {"typedef", TK_TYPEDEF}, {"enum", TK_ENUM},
            {"_Generic", TK_GENERIC}, {"struct", TK_STRUCT}, {"union", TK_UNION},
            {"signed", TK_SIGNED}, {"unsigned", TK_UNSIGNED}, {"short", TK_SHORT},
            {"long", TK_LONG}, {"float", TK_FLOAT}, {"_Bool", TK_BOOL},
            {"volatile", TK_VOLATILE}, {"restrict", TK_RESTRICT}, {"extern", TK_EXTERN},
            {"register", TK_REGISTER}, {"inline", TK_INLINE}, {"auto", TK_AUTO}
        };
        for (size_t i = 0; i < sizeof keywords / sizeof keywords[0]; ++i)
            if (strlen(keywords[i].name) == token.length &&
                !memcmp(token.start, keywords[i].name, token.length)) token.kind = keywords[i].kind;
        return token;
    }
    if (isdigit(first) || (first == '.' && isdigit((unsigned char)lexer->cursor[1]))) {
        int hex = first == '0' && (lexer->cursor[1] == 'x' || lexer->cursor[1] == 'X');
        int real = first == '.';
        while (isalnum((unsigned char)*lexer->cursor) || *lexer->cursor == '.' ||
               *lexer->cursor == '_') {
            char c = *lexer->cursor;
            if (c == '.' || (hex ? (c == 'p' || c == 'P') : (c == 'e' || c == 'E'))) real = 1;
            advance(lexer);
            if ((hex ? (c == 'p' || c == 'P') : (c == 'e' || c == 'E')) &&
                (*lexer->cursor == '+' || *lexer->cursor == '-')) advance(lexer);
        }
        token.length = (size_t)(lexer->cursor - token.start);
        char *end = NULL;
        errno = 0;
        if (real) {
            double value = strtod(token.start, &end);
            if (!suffix_ends(end, lexer->cursor, "flFL") ||
                (hex && !memchr(token.start, 'p', token.length) && !memchr(token.start, 'P', token.length)))
                return bad(lexer, token, "invalid or unsupported floating-point literal");
            if (errno == ERANGE || !isfinite(value)) return bad(lexer, token, "floating-point literal out of range");
            if (memchr(end, 'l', (size_t)(lexer->cursor - end)) || memchr(end, 'L', (size_t)(lexer->cursor - end)))
                return bad(lexer, token, "long double is not supported");
            int single = end != lexer->cursor;
            token.kind = TK_REAL;
            token.value = (CtValue){.type = single ? CT_FLOAT : CT_DOUBLE,
                                    .as.real = single ? (double)(float)value : value};
        } else {
            unsigned long long value = strtoull(token.start, &end, 0);
            if (!suffix_ends(end, lexer->cursor, "uUlL")) return bad(lexer, token, "invalid or unsupported integer literal");
            if (errno == ERANGE) return bad(lexer, token, "integer literal exceeds unsigned long long");
            int unsigned_suffix = 0, long_suffix = 0;
            for (const char *cursor = end; cursor != lexer->cursor; ++cursor)
                if (*cursor == 'u' || *cursor == 'U') unsigned_suffix = 1;
                else ++long_suffix;
            CtType type = literal_type(value, unsigned_suffix, long_suffix, hex || *token.start == '0');
            if (type == CT_VOID) return bad(lexer, token, "no integer type can hold the literal");
            token.kind = TK_INTEGER;
            token.value = (CtValue){.type = type, .as.unsigned_integer = value};
        }
        return token;
    }
    static const struct { const char *text; int kind; } operators[] = {
        {"<<=", TK_SHL_ASSIGN}, {">>=", TK_SHR_ASSIGN}, {"...", TK_ELLIPSIS},
        {"++", TK_INCREMENT}, {"--", TK_DECREMENT}, {"->", TK_ARROW},
        {"==", TK_EQ}, {"!=", TK_NE}, {"<=", TK_LE}, {">=", TK_GE},
        {"&&", TK_AND}, {"||", TK_OR}, {"<<", TK_SHL}, {">>", TK_SHR},
        {"+=", TK_ADD_ASSIGN}, {"-=", TK_SUB_ASSIGN}, {"*=", TK_MUL_ASSIGN},
        {"/=", TK_DIV_ASSIGN}, {"%=", TK_MOD_ASSIGN}, {"&=", TK_AND_ASSIGN},
        {"|=", TK_OR_ASSIGN}, {"^=", TK_XOR_ASSIGN}
    };
    for (size_t i = 0; i < sizeof operators / sizeof operators[0]; ++i) {
        size_t length = strlen(operators[i].text);
        if (!strncmp(lexer->cursor, operators[i].text, length)) {
            token.kind = operators[i].kind;
            token.length = length;
            for (size_t j = 0; j < length; ++j) advance(lexer);
            return token;
        }
    }
    advance(lexer);
    token.length = 1;
    token.kind = first;
    if (!strchr("+-*/%~!&|^<>=(){}[]?:;,.", first))
        return bad(lexer, token, "unsupported character or language feature");
    return token;
}

char *lexer_decode(Token token, size_t *length, CtError *error) {
    char *result = malloc(token.length + 1);
    const char *message = NULL;
    if (!result) message = "out of memory";
    *length = 0;
    for (size_t i = 1; !message && i + 1 < token.length; ++i) {
        unsigned char c = (unsigned char)token.start[i];
        if (c == '\\') {
            c = (unsigned char)token.start[++i];
            switch (c) {
                case '\n': continue;
                case 'a': c = '\a'; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                case 'v': c = '\v'; break;
                case '\\': case '\'': case '"': case '?': break;
                default: {
                    unsigned value = 0, digits = 0;
                    if (c == 'x') {
                        while (i + 2 < token.length && isxdigit((unsigned char)token.start[i + 1])) {
                            unsigned char digit = (unsigned char)token.start[++i];
                            value = value * 16u + (isdigit(digit) ? (unsigned)(digit - '0') : (unsigned)(tolower(digit) - 'a' + 10));
                            ++digits;
                            if (value > UCHAR_MAX) { message = "hex escape exceeds one byte"; break; }
                        }
                        if (!digits) message = "hex escape requires a digit";
                    } else if (c >= '0' && c <= '7') {
                        value = (unsigned)(c - '0');
                        for (digits = 1; digits < 3 && i + 2 < token.length &&
                             token.start[i + 1] >= '0' && token.start[i + 1] <= '7'; ++digits)
                            value = value * 8u + (unsigned)(token.start[++i] - '0');
                        if (value > UCHAR_MAX) message = "octal escape exceeds one byte";
                    } else message = "unknown escape sequence";
                    c = (unsigned char)value;
                    break;
                }
            }
        }
        result[(*length)++] = (char)c;
    }
    if (message) {
        free(result);
        *error = (CtError){.line = token.line, .column = token.column};
        (void)snprintf(error->message, sizeof error->message, "%s", message);
        return NULL;
    }
    result[*length] = '\0';
    return result;
}
