#define _POSIX_C_SOURCE 200809L

#include "lexer.h"
#include "parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void report(const char *what, const char *detail) {
    fprintf(stderr, "Failed: %s (%s)\n", what, detail);
    ++failures;
}

static void expect_tokens(const char *source, const int *kinds, size_t count) {
    Lexer lexer;
    lexer_init(&lexer, source);
    for (size_t i = 0; i < count; ++i) {
        Token token = lexer_next(&lexer);
        if (token.kind != kinds[i]) {
            char detail[128];
            (void)snprintf(detail, sizeof detail, "token %zu was %d, expected %d", i, token.kind, kinds[i]);
            report(source, detail);
            return;
        }
    }
    if (lexer_next(&lexer).kind != TK_EOF) report(source, "trailing tokens");
}

static void expect_integer(const char *source, int value) {
    Lexer lexer;
    lexer_init(&lexer, source);
    Token token = lexer_next(&lexer);
    if (token.kind != TK_INTEGER || token.value.as.integer != value) report(source, "integer literal");
}

static void expect_real(const char *source, double value) {
    Lexer lexer;
    lexer_init(&lexer, source);
    Token token = lexer_next(&lexer);
    if (token.kind != TK_REAL || token.value.as.real != value) report(source, "floating-point literal");
}

static void expect_literal_type(const char *source, CtType expected) {
    Lexer lexer;
    lexer_init(&lexer, source);
    Token token = lexer_next(&lexer);
    if ((token.kind != TK_INTEGER && token.kind != TK_REAL) || token.value.type != expected) {
        char detail[128], name[64];
        ct_type_name(token.value.type, name, sizeof name);
        (void)snprintf(detail, sizeof detail, "literal typed %s", name);
        report(source, detail);
    }
}

static void expect_lexer_error(const char *source, int incomplete) {
    Lexer lexer;
    lexer_init(&lexer, source);
    Token token;
    do { token = lexer_next(&lexer); } while (token.kind != TK_ERROR && token.kind != TK_EOF);
    if (token.kind != TK_ERROR) report(source, "expected a lexer error");
    else if (lexer.incomplete != incomplete) report(source, "wrong incomplete flag");
}

static void expect_string(const char *source, const char *text, size_t length) {
    Lexer lexer;
    lexer_init(&lexer, source);
    Token token = lexer_next(&lexer);
    CtError error = {0};
    size_t decoded_length = 0;
    char *decoded = token.kind == TK_STRING ? lexer_decode(token, &decoded_length, &error) : NULL;
    if (!decoded) report(source, "could not decode");
    else if (decoded_length != length || memcmp(decoded, text, length)) report(source, "decoded contents");
    free(decoded);
}

static void expect_positions(const char *source, size_t line, size_t column) {
    Lexer lexer;
    lexer_init(&lexer, source);
    Token token = lexer_next(&lexer);
    while (token.kind != TK_EOF && token.kind != TK_ERROR) {
        Token following = lexer_next(&lexer);
        if (following.kind == TK_EOF || following.kind == TK_ERROR) break;
        token = following;
    }
    if (token.line != line || token.column != column) {
        char detail[128];
        (void)snprintf(detail, sizeof detail, "last token at %zu:%zu, expected %zu:%zu",
                       token.line, token.column, line, column);
        report(source, detail);
    }
}

static void expect_parse(const char *source, CtStatus expected) {
    Unit *unit = NULL;
    CtError error = {0};
    CtStatus status = parse(source, &unit, &error);
    if (status != expected) {
        char detail[256];
        (void)snprintf(detail, sizeof detail, "status %d, expected %d: %s", status, expected, error.message);
        report(source, detail);
    }
    if (status == CT_OK && !unit) report(source, "missing unit");
    if (status != CT_OK && unit) report(source, "unit produced for a failed parse");
    unit_destroy(unit);
}

/* The dumped tree is the parser's observable shape, so assert on it directly. */
static void expect_tree(const char *source, const char *expected) {
    Unit *unit = NULL;
    CtError error = {0};
    if (parse(source, &unit, &error) != CT_OK) { report(source, error.message); return; }
    char *text = NULL;
    size_t size = 0;
    FILE *sink = open_memstream(&text, &size);
    if (!sink) { report(source, "could not open a stream"); unit_destroy(unit); return; }
    dump_ast(unit, sink);
    fclose(sink);
    if (!text || !strstr(text, expected)) {
        fprintf(stderr, "Failed: %s\nexpected to contain: %s\ngot:\n%s", source, expected, text ? text : "");
        ++failures;
    }
    free(text);
    unit_destroy(unit);
}

int main(void) {
    int declaration[] = {TK_INT, TK_NAME, '=', TK_INTEGER, ';'};
    expect_tokens("int x = 3;", declaration, 5);
    int operators[] = {TK_SHL_ASSIGN, TK_SHR_ASSIGN, TK_ARROW, TK_ELLIPSIS, TK_INCREMENT,
                       TK_DECREMENT, TK_LE, TK_GE, TK_EQ, TK_NE, TK_AND, TK_OR, '.', '-', '>'};
    expect_tokens("<<= >>= -> ... ++ -- <= >= == != && || . - >", operators, 15);
    int keywords[] = {TK_STRUCT, TK_UNION, TK_TYPEDEF, TK_ENUM, TK_GENERIC, TK_SIZEOF, TK_ALIGNOF,
                      TK_SWITCH, TK_CASE, TK_DEFAULT, TK_GOTO, TK_STATIC, TK_CONST, TK_DO,
                      TK_SIGNED, TK_UNSIGNED, TK_SHORT, TK_LONG, TK_FLOAT, TK_BOOL,
                      TK_VOLATILE, TK_RESTRICT, TK_EXTERN, TK_REGISTER, TK_INLINE, TK_AUTO};
    expect_tokens("struct union typedef enum _Generic sizeof _Alignof switch case default goto static const do "
                  "signed unsigned short long float _Bool volatile restrict extern register inline auto",
                  keywords, 26);
    int commented[] = {TK_INT, TK_NAME, ';'};
    expect_tokens("int /* here */ value; // and here", commented, 3);

    expect_integer("42", 42);
    expect_integer("0x2A", 42);
    expect_integer("052", 42);
    expect_integer("42u", 42);
    expect_integer("42UL", 42);
    expect_integer("'*'", '*');
    expect_integer("'\\n'", '\n');
    expect_integer("'\\x41'", 'A');
    expect_integer("'\\101'", 'A');
    expect_real("1.5", 1.5);
    expect_real("1e3", 1000.0);
    expect_real(".5f", 0.5);
    expect_real("0x1p3", 8.0);

    expect_literal_type("1", CT_INT);
    expect_literal_type("2147483648", CT_LONG);
    expect_literal_type("1u", CT_UINT);
    expect_literal_type("1l", CT_LONG);
    expect_literal_type("1ul", CT_ULONG);
    expect_literal_type("1ll", CT_LLONG);
    expect_literal_type("0xffffffff", CT_UINT);
    expect_literal_type("1.5", CT_DOUBLE);
    expect_literal_type("1.5f", CT_FLOAT);

    expect_string("\"a\\tb\"", "a\tb", 3);
    expect_string("\"\\0embedded\"", "\0embedded", 9);

    expect_lexer_error("\"unterminated", 1);
    expect_lexer_error("/* unterminated", 1);
    expect_lexer_error("18446744073709551616", 0);
    expect_lexer_error("'ab'", 0);
    expect_lexer_error("\"\\q\"", 0);
    expect_lexer_error("1.2.3", 0);
    expect_lexer_error("@", 0);

    expect_positions("int x;\nint y;\n", 2, 6);
    expect_positions("/* two\nlines */ x;", 2, 11);

    expect_parse("int x = 1;", CT_OK);
    expect_parse("int f(int a, int b) { return a + b; }", CT_OK);
    expect_parse("struct Point { int x, y; }; struct Point p = { .y = 2 };", CT_OK);
    expect_parse("int (*table[4])(int, char **);", CT_OK);
    expect_parse("typedef int (*Handler)(void); Handler h;", CT_OK);
    expect_parse("int matrix[2][3] = {{1,2,3},{4,5,6}};", CT_OK);
    expect_parse("union U { int i; char c[4]; } u;", CT_OK);
    expect_parse("enum Colour { RED, GREEN = 4, BLUE };", CT_OK);
    expect_parse("void f(void) { struct Local { int x; } value; value.x = 1; }", CT_OK);

    expect_parse("int x = ", CT_INCOMPLETE);
    expect_parse("int f(void) {", CT_INCOMPLETE);
    expect_parse("if (1", CT_INCOMPLETE);
    expect_parse("1 +", CT_INCOMPLETE);

    expect_parse("int 3x;", CT_ERROR);
    expect_parse("1 + + ;", CT_ERROR);
    expect_parse("struct Unknown value;", CT_ERROR);
    expect_parse("int a[0];", CT_ERROR);
    expect_parse("int a[];", CT_ERROR);
    expect_parse("int n; int a[n];", CT_ERROR);
    expect_parse("struct Empty { };", CT_ERROR);
    expect_parse("struct S { int x; int x; };", CT_ERROR);
    expect_parse("return 1;", CT_ERROR);
    expect_parse("break;", CT_ERROR);
    expect_parse("case 1:", CT_ERROR);
    expect_parse("void f(void) { int g(void); }", CT_ERROR);
    expect_parse("1 = 2;", CT_ERROR);

    expect_tree("int *p;", "declaration p : int *");
    expect_tree("int m[2][3];", "declaration m : int[2][3]");
    expect_tree("int (*f)(char);", "declaration f : int (*)(char)");
    expect_tree("int *g(char);", "function g : int *(char)");
    expect_tree("a->b;", "member b");
    expect_tree("(int){0};", "compound ( : int");
    expect_tree("int v[3] = { [2] = 9 };", "designated");
    expect_tree("unsigned long long counter;", "declaration counter : unsigned long long");
    expect_tree("signed char level;", "declaration level : signed char");
    expect_tree("long unsigned int mixed;", "declaration mixed : unsigned long");
    expect_tree("volatile const float ratio;", "declaration ratio : float");
    expect_parse("long double wide;", CT_ERROR);
    expect_parse("short long confused;", CT_ERROR);
    expect_parse("unsigned double wrong;", CT_ERROR);

    if (failures) fprintf(stderr, "%d lexer/parser checks failed\n", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
