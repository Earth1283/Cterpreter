#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAPE 4096
#define NESTING 128

static const char *const catalogue[] = {
    "hello", "++++++++[>++++[>++>+++>+++>+<<<<-]>+>+>->>+[<]<-]>>.>---.+++++++..+++."
             ">>.<-.<.+++.------.--------.>>+.>++.",
    "squares", "++++[>+++++<-]>[<+++++>-]+<+[>[>+>+<<-]++>>[<<+>>-]>>>[-]++>[-]+"
               ">>>+[[-]++++++>>>]<<<[[<++++++++<++>>-]+<.<[>----<-]<]<<[>>>>>[>>>[-]+++++++++<[>-<-]"
               "+++++++++>[-[<->-]+[<<<]]<[>+<-]>]<<-]<<-]",
    "alphabet", "++++++++[>++++++++<-]>+>++++++++++++++++++++++++++[<.+>-]>++++++++++.",
    "triangle", "++++++++[>+>++++<<-]>++>>+<[-[>>+<<-]+>>]>+[-<<<[->[+[-]+>++>>>-<<]<[<]>>++++++"
                "[<<+++++>>-]+<<++.[-]<<]>.>+[>>]>+]",
    NULL, NULL
};

static int run(const char *program, unsigned char *tape) {
    size_t length = strlen(program);
    size_t stack[NESTING], targets[8192];
    size_t depth = 0;
    if (length > 8192) { fprintf(stderr, "program too long\n"); return 1; }
    for (size_t i = 0; i < length; i++) {
        if (program[i] == '[') {
            if (depth == NESTING) { fprintf(stderr, "brackets nested too deeply\n"); return 1; }
            stack[depth++] = i;
        } else if (program[i] == ']') {
            if (!depth) { fprintf(stderr, "unmatched ] at %zu\n", i); return 1; }
            size_t open = stack[--depth];
            targets[open] = i;
            targets[i] = open;
        }
    }
    if (depth) { fprintf(stderr, "unmatched [ at %zu\n", stack[depth - 1]); return 1; }

    size_t cell = 0;
    for (size_t ip = 0; ip < length; ip++) {
        char op = program[ip];
        if (op == '>') { if (++cell == TAPE) cell = 0; }
        else if (op == '<') { cell = cell ? cell - 1 : TAPE - 1; }
        else if (op == '+') tape[cell]++;
        else if (op == '-') tape[cell]--;
        else if (op == '.') putchar(tape[cell]);
        else if (op == ',') { int c = getchar(); tape[cell] = (unsigned char)(c == EOF ? 0 : c); }
        else if (op == '[' && !tape[cell]) ip = targets[ip];
        else if (op == ']' && tape[cell]) ip = targets[ip];
    }
    return 0;
}

int main(int argc, char **argv) {
    const char *program = NULL;
    const char *choice = argc > 1 ? argv[1] : "hello";

    for (int i = 0; catalogue[i]; i += 2)
        if (!strcmp(catalogue[i], choice)) program = catalogue[i + 1];
    if (!program) program = choice;

    unsigned char tape[TAPE];
    memset(tape, 0, TAPE);
    int status = run(program, tape);
    putchar('\n');

    int used = 0;
    for (int i = 0; i < TAPE; i++) used += tape[i] != 0;
    fprintf(stderr, "%d non-zero cells remain\n", used);
    return status;
}
