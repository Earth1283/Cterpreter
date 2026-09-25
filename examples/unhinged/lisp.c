#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A Lisp with closures, written in the subset of C that Cterpreter runs. Every
 * value is an index into one pool of cells, and nothing is ever freed. */

enum { NIL, NUMBER, SYMBOL, PAIR, PRIMITIVE, CLOSURE };
enum { F_ADD, F_SUB, F_MUL, F_MOD, F_LESS, F_EQUAL, F_CAR, F_CDR, F_CONS, F_NULL, F_LIST };

typedef struct { int kind; long number; int car, cdr; } Cell;

#define CELLS 40000
static Cell cells[CELLS];
static int used = 1;
static char names[256][16];
static int symbols[256], name_count;
static int global, s_quote, s_if, s_define, s_lambda, s_true;

static int make(int kind, long number, int car, int cdr) {
    if (used == CELLS) { fprintf(stderr, "lisp: out of cells\n"); exit(1); }
    cells[used] = (Cell){kind, number, car, cdr};
    return used++;
}

static int cons(int car, int cdr) { return make(PAIR, 0, car, cdr); }
static int car(int cell) { return cells[cell].kind == PAIR ? cells[cell].car : NIL; }
static int cdr(int cell) { return cells[cell].kind == PAIR ? cells[cell].cdr : NIL; }

/* Symbols are interned, so two are the same symbol exactly when they are the same cell. */
static int symbol(const char *name, size_t length) {
    for (int i = 0; i < name_count; i++)
        if (strlen(names[i]) == length && !strncmp(names[i], name, length)) return symbols[i];
    memcpy(names[name_count], name, length);
    names[name_count][length] = '\0';
    symbols[name_count] = make(SYMBOL, name_count, 0, 0);
    return symbols[name_count++];
}

static const char *cursor;

static int read_form(void);

static int read_list(void) {
    while (*cursor == ' ' || *cursor == '\n') cursor++;
    if (*cursor == ')') { cursor++; return NIL; }
    int head = read_form();
    return cons(head, read_list());
}

static int read_form(void) {
    while (*cursor == ' ' || *cursor == '\n') cursor++;
    if (*cursor == '(') { cursor++; return read_list(); }
    if (*cursor == '\'') {
        cursor++;
        return cons(s_quote, cons(read_form(), NIL));
    }
    const char *start = cursor;
    while (*cursor && *cursor != ' ' && *cursor != '\n' && *cursor != '(' && *cursor != ')') cursor++;
    char *end;
    long value = strtol(start, &end, 10);
    if (end == cursor && end != start && (start[0] != '-' || cursor - start > 1)) return make(NUMBER, value, 0, 0);
    return symbol(start, (size_t)(cursor - start));
}

/* Local bindings first, then the globals as they are now, so that a definition
 * can refer to itself and to anything defined after it. */
static int lookup(int name, int environment) {
    for (; environment != NIL; environment = cdr(environment))
        if (car(car(environment)) == name) return cdr(car(environment));
    for (environment = cdr(global); environment != NIL; environment = cdr(environment))
        if (car(car(environment)) == name) return cdr(car(environment));
    fprintf(stderr, "lisp: unbound %s\n", names[cells[name].number]);
    exit(1);
}

static int truthy(int value) { return value != NIL; }

static int evaluate(int form, int environment);

static int apply(int function, int arguments) {
    if (cells[function].kind == PRIMITIVE) {
        int a = car(arguments), b = car(cdr(arguments));
        switch (cells[function].number) {
            case F_ADD: return make(NUMBER, cells[a].number + cells[b].number, 0, 0);
            case F_SUB: return make(NUMBER, cells[a].number - cells[b].number, 0, 0);
            case F_MUL: return make(NUMBER, cells[a].number * cells[b].number, 0, 0);
            case F_MOD: return make(NUMBER, cells[a].number % cells[b].number, 0, 0);
            case F_LESS: return cells[a].number < cells[b].number ? s_true : NIL;
            case F_EQUAL: return cells[a].number == cells[b].number ? s_true : NIL;
            case F_CAR: return car(a);
            case F_CDR: return cdr(a);
            case F_CONS: return cons(a, b);
            case F_NULL: return a == NIL ? s_true : NIL;
            default: return arguments;
        }
    }
    if (cells[function].kind != CLOSURE) { fprintf(stderr, "lisp: not a function\n"); exit(1); }
    int lambda = cells[function].car, scope = cells[function].cdr;
    for (int parameter = car(cdr(lambda)); parameter != NIL; parameter = cdr(parameter), arguments = cdr(arguments))
        scope = cons(cons(car(parameter), car(arguments)), scope);
    int result = NIL;
    for (int body = cdr(cdr(lambda)); body != NIL; body = cdr(body)) result = evaluate(car(body), scope);
    return result;
}

static int evaluate(int form, int environment) {
    switch (cells[form].kind) {
        case NIL: case NUMBER: case PRIMITIVE: case CLOSURE: return form;
        case SYMBOL: return lookup(form, environment);
        default: break;
    }
    int head = car(form);
    if (head == s_quote) return car(cdr(form));
    if (head == s_if)
        return evaluate(truthy(evaluate(car(cdr(form)), environment)) ? car(cdr(cdr(form))) : car(cdr(cdr(cdr(form)))), environment);
    if (head == s_lambda) return make(CLOSURE, 0, form, environment);
    if (head == s_define) {
        int binding = cons(car(cdr(form)), NIL);
        cells[global].cdr = cons(binding, cells[global].cdr);
        cells[binding].cdr = evaluate(car(cdr(cdr(form))), environment);
        return car(cdr(form));
    }
    int function = evaluate(head, environment), arguments = NIL, *tail = &arguments;
    for (int argument = cdr(form); argument != NIL; argument = cdr(argument)) {
        int value = evaluate(car(argument), environment);
        *tail = cons(value, NIL);
        tail = &cells[*tail].cdr;
    }
    return apply(function, arguments);
}

static void print(int value) {
    switch (cells[value].kind) {
        case NIL: printf("()"); break;
        case NUMBER: printf("%ld", cells[value].number); break;
        case SYMBOL: printf("%s", names[cells[value].number]); break;
        case PRIMITIVE: printf("#<primitive>"); break;
        case CLOSURE: printf("#<closure>"); break;
        default:
            putchar('(');
            for (;;) {
                print(car(value));
                value = cdr(value);
                if (cells[value].kind != PAIR) break;
                putchar(' ');
            }
            if (value != NIL) { printf(" . "); print(value); }
            putchar(')');
    }
}

static const char program[] =
    "(define fact (lambda (n) (if (< n 2) 1 (* n (fact (- n 1))))))\n"
    "(fact 12)\n"
    "(define map (lambda (f xs) (if (null? xs) '() (cons (f (car xs)) (map f (cdr xs))))))\n"
    "(define range (lambda (a b) (if (< a b) (cons a (range (+ a 1) b)) '())))\n"
    "(map (lambda (x) (* x x)) (range 1 11))\n"
    "(define filter (lambda (keep xs) (if (null? xs) '()\n"
    "  (if (keep (car xs)) (cons (car xs) (filter keep (cdr xs))) (filter keep (cdr xs))))))\n"
    "(define sieve (lambda (xs) (if (null? xs) '()\n"
    "  (cons (car xs) (sieve (filter (lambda (n) (null? (= (mod n (car xs)) 0))) (cdr xs)))))))\n"
    "(sieve (range 2 30))\n"
    "(define compose (lambda (f g) (lambda (x) (f (g x)))))\n"
    "((compose (lambda (x) (+ x 1)) (lambda (x) (* x 2))) 20)\n"
    "(define adder (lambda (n) (lambda (x) (+ x n))))\n"
    "(map (adder 100) '(1 2 3))\n"
    "(define Y (lambda (f) ((lambda (x) (f (lambda (v) ((x x) v))))\n"
    "                       (lambda (x) (f (lambda (v) ((x x) v)))))))\n"
    "((Y (lambda (self) (lambda (n) (if (< n 2) 1 (* n (self (- n 1))))))) 10)\n"
    "(list 'this 'is (list 'a 'nested) 'list)\n"
    "(cons 'improper 'pair)\n";

int main(void) {
    static const char *const primitive_names[] = {"+", "-", "*", "mod", "<", "=", "car", "cdr", "cons", "null?", "list"};
    s_quote = symbol("quote", 5);
    s_if = symbol("if", 2);
    s_define = symbol("define", 6);
    s_lambda = symbol("lambda", 6);
    s_true = symbol("t", 1);
    global = cons(NIL, NIL);
    for (int i = 0; i < (int)(sizeof primitive_names / sizeof primitive_names[0]); i++) {
        int name = symbol(primitive_names[i], strlen(primitive_names[i]));
        cells[global].cdr = cons(cons(name, make(PRIMITIVE, i, 0, 0)), cells[global].cdr);
    }
    cursor = program;
    for (;;) {
        while (*cursor == ' ' || *cursor == '\n') cursor++;
        if (!*cursor) break;
        const char *start = cursor;
        int form = read_form();
        printf("%.*s\n  => ", (int)(cursor - start), start);
        print(evaluate(form, NIL));
        putchar('\n');
    }
    return 0;
}
