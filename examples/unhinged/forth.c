#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A Forth that compiles colon definitions to threaded code, then runs a
 * program written in it. The program is at the bottom of this file. */

enum { OP_LIT, OP_CALL, OP_PRIMITIVE, OP_BRANCH, OP_ZBRANCH, OP_DO, OP_LOOP, OP_INDEX, OP_EXIT, OP_PRINT };
enum {
    P_ADD, P_SUB, P_MUL, P_DIV, P_MOD, P_DUP, P_DROP, P_SWAP, P_OVER, P_ROT, P_DOT, P_EMIT, P_CR,
    P_EQUAL, P_LESS, P_GREATER, P_AT_LEAST, P_ZERO, P_AND, P_OR, P_TWO_DUP, P_TWO_DROP, P_SPACE, P_SHOW
};
static const char *const primitives[] = {
    "+", "-", "*", "/", "mod", "dup", "drop", "swap", "over", "rot", ".", "emit", "cr",
    "=", "<", ">", ">=", "0=", "and", "or", "2dup", "2drop", "space", ".s"
};

typedef struct { int op; long argument; } Cell;
typedef struct { char name[32]; int primitive, start; } Word;

static long stack[256], loops[128];
static int depth, loop_depth;
static Cell code[4096];
static int here;
static Word words[256];
static int word_count;
static char strings[4096];
static int string_top;

static void fail(const char *message, const char *detail) {
    fprintf(stderr, "forth: %s %s\n", message, detail);
    exit(1);
}

static void push(long value) {
    if (depth == 256) fail("stack overflow", "");
    stack[depth++] = value;
}

static long pop(void) {
    if (!depth) fail("stack underflow", "");
    return stack[--depth];
}

static int find(const char *name) {
    for (int i = word_count - 1; i >= 0; i--)
        if (!strcmp(words[i].name, name)) return i;
    return -1;
}

static void primitive(int id) {
    long a, b, c;
    switch (id) {
        case P_ADD: b = pop(); a = pop(); push(a + b); break;
        case P_SUB: b = pop(); a = pop(); push(a - b); break;
        case P_MUL: b = pop(); a = pop(); push(a * b); break;
        case P_DIV: b = pop(); a = pop(); if (!b) fail("division by zero", ""); push(a / b); break;
        case P_MOD: b = pop(); a = pop(); if (!b) fail("division by zero", ""); push(a % b); break;
        case P_DUP: a = pop(); push(a); push(a); break;
        case P_DROP: (void)pop(); break;
        case P_SWAP: b = pop(); a = pop(); push(b); push(a); break;
        case P_OVER: b = pop(); a = pop(); push(a); push(b); push(a); break;
        case P_ROT: c = pop(); b = pop(); a = pop(); push(b); push(c); push(a); break;
        case P_DOT: printf("%ld ", pop()); break;
        case P_EMIT: putchar((int)pop()); break;
        case P_CR: putchar('\n'); break;
        case P_EQUAL: b = pop(); a = pop(); push(-(a == b)); break;
        case P_LESS: b = pop(); a = pop(); push(-(a < b)); break;
        case P_GREATER: b = pop(); a = pop(); push(-(a > b)); break;
        case P_AT_LEAST: b = pop(); a = pop(); push(-(a >= b)); break;
        case P_ZERO: push(-(pop() == 0)); break;
        case P_AND: b = pop(); a = pop(); push(a & b); break;
        case P_OR: b = pop(); a = pop(); push(a | b); break;
        case P_TWO_DUP: b = pop(); a = pop(); push(a); push(b); push(a); push(b); break;
        case P_TWO_DROP: (void)pop(); (void)pop(); break;
        case P_SPACE: putchar(' '); break;
        case P_SHOW:
            printf("<%d> ", depth);
            for (int i = 0; i < depth; i++) printf("%ld ", stack[i]);
            break;
    }
}

static void run(int ip) {
    for (;;) {
        Cell cell = code[ip++];
        switch (cell.op) {
            case OP_LIT: push(cell.argument); break;
            case OP_CALL: run(words[cell.argument].start); break;
            case OP_PRIMITIVE: primitive((int)cell.argument); break;
            case OP_BRANCH: ip = (int)cell.argument; break;
            case OP_ZBRANCH: if (!pop()) ip = (int)cell.argument; break;
            case OP_DO: {
                long index = pop(), limit = pop();
                loops[loop_depth++] = limit;
                loops[loop_depth++] = index;
                break;
            }
            case OP_LOOP:
                if (++loops[loop_depth - 1] < loops[loop_depth - 2]) ip = (int)cell.argument;
                else loop_depth -= 2;
                break;
            case OP_INDEX: push(loops[loop_depth - 1]); break;
            case OP_PRINT: fputs(strings + cell.argument, stdout); break;
            case OP_EXIT: return;
        }
    }
}

static const char *cursor;

static int next_token(char *token) {
    for (;;) {
        while (*cursor == ' ' || *cursor == '\n' || *cursor == '\t') cursor++;
        if (*cursor == '\\') { while (*cursor && *cursor != '\n') cursor++; continue; }
        if (cursor[0] == '(' && cursor[1] == ' ') { while (*cursor && *cursor != ')') cursor++; if (*cursor) cursor++; continue; }
        break;
    }
    if (!*cursor) return 0;
    int length = 0;
    while (*cursor && *cursor != ' ' && *cursor != '\n' && *cursor != '\t' && length < 31) token[length++] = *cursor++;
    token[length] = '\0';
    return 1;
}

static void emit(int op, long argument) {
    if (here == 4096) fail("out of code space", "");
    code[here++] = (Cell){op, argument};
}

static int number(const char *token, long *value) {
    char *end;
    *value = strtol(token, &end, 10);
    return *token && !*end && (*token != '-' || token[1]);
}

/* Stores the text up to the closing quote and returns where it starts. */
static int quoted(void) {
    int start = string_top;
    if (*cursor == ' ') cursor++;
    while (*cursor && *cursor != '"') strings[string_top++] = *cursor++;
    if (*cursor) cursor++;
    strings[string_top++] = '\0';
    return start;
}

static void compile_word(void) {
    char token[32];
    int patches[64], pending = 0;
    if (!next_token(token)) fail("expected a name after", ":");
    Word *word = &words[word_count++];
    strcpy(word->name, token);
    word->primitive = -1;
    word->start = here;
    while (next_token(token)) {
        long value;
        int found;
        if (!strcmp(token, ";")) { emit(OP_EXIT, 0); return; }
        if (!strcmp(token, "if") || !strcmp(token, "while")) { patches[pending++] = here; emit(OP_ZBRANCH, -1); }
        else if (!strcmp(token, "else")) {
            int branch = here;
            emit(OP_BRANCH, -1);
            code[patches[--pending]].argument = here;
            patches[pending++] = branch;
        } else if (!strcmp(token, "then")) code[patches[--pending]].argument = here;
        else if (!strcmp(token, "begin")) patches[pending++] = here;
        else if (!strcmp(token, "repeat")) {
            int exit_branch = patches[--pending], start = patches[--pending];
            emit(OP_BRANCH, start);
            code[exit_branch].argument = here;
        } else if (!strcmp(token, "do")) { emit(OP_DO, 0); patches[pending++] = here; }
        else if (!strcmp(token, "loop")) emit(OP_LOOP, patches[--pending]);
        else if (!strcmp(token, "i")) emit(OP_INDEX, 0);
        else if (!strcmp(token, "exit")) emit(OP_EXIT, 0);
        else if (!strcmp(token, ".\"")) emit(OP_PRINT, quoted());
        else if ((found = find(token)) >= 0) {
            if (words[found].primitive >= 0) emit(OP_PRIMITIVE, words[found].primitive);
            else emit(OP_CALL, found);
        } else if (number(token, &value)) emit(OP_LIT, value);
        else fail("unknown word", token);
    }
    fail("unterminated definition", word->name);
}

static void interpret(const char *source) {
    char token[32];
    cursor = source;
    while (next_token(token)) {
        long value;
        int found;
        if (!strcmp(token, ":")) compile_word();
        else if (!strcmp(token, ".\"")) fputs(strings + quoted(), stdout);
        else if ((found = find(token)) >= 0) {
            if (words[found].primitive >= 0) primitive(words[found].primitive);
            else run(words[found].start);
        } else if (number(token, &value)) push(value);
        else fail("unknown word", token);
    }
}

static const char program[] =
    "\\ Everything below is Forth, running inside C, running inside Cterpreter.\n"
    ": square ( n -- n*n ) dup * ;\n"
    ": cube dup square * ;\n"
    ": factorial dup 1 > if dup 1 - factorial * then ;\n"
    ": fib dup 2 < if exit then dup 1 - fib swap 2 - fib + ;\n"
    ": stars 0 do 42 emit loop ;\n"
    ": triangle 1 + 1 do i stars cr loop ;\n"
    ": prime? dup 2 < if drop 0 exit then 2\n"
    "    begin 2dup dup * >= while 2dup mod 0= if 2drop 0 exit then 1 + repeat 2drop -1 ;\n"
    ": primes .\" primes: \" 2 do i prime? if i . then loop cr ;\n"
    ": fizz dup 15 mod 0= if .\" FizzBuzz \" drop exit then\n"
    "    dup 3 mod 0= if .\" Fizz \" drop exit then\n"
    "    dup 5 mod 0= if .\" Buzz \" drop exit then . ;\n"
    ": fizzbuzz 1 + 1 do i fizz loop cr ;\n"
    ": collatz ( n -- steps ) 0 swap begin dup 1 > while\n"
    "    dup 2 mod if 3 * 1 + else 2 / then swap 1 + swap repeat drop ;\n"
    ".\" squares and cubes: \" 7 square . 3 cube . cr\n"
    ".\" 12! = \" 12 factorial . cr\n"
    ".\" fib(14) = \" 14 fib . cr\n"
    ".\" 27 reaches 1 in \" 27 collatz . .\" steps\" cr\n"
    "50 primes\n"
    "15 fizzbuzz\n"
    "5 triangle\n"
    "1 2 3 rot .s cr\n";

int main(void) {
    for (int i = 0; i < (int)(sizeof primitives / sizeof primitives[0]); i++) {
        strcpy(words[word_count].name, primitives[i]);
        words[word_count++].primitive = i;
    }
    interpret(program);
    return 0;
}
