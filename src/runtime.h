#ifndef CT_RUNTIME_H
#define CT_RUNTIME_H

#include "parser.h"
#include "memory.h"
#include "preprocessor.h"

typedef struct Symbol Symbol;
struct Symbol {
    char *name;
    size_t length, count;
    CtValue value;
    uint64_t address;
    int is_array, is_static, is_const, enum_constant;
    Node *function;
    Symbol *next;
};

typedef struct Scope Scope;
struct Scope { Symbol *symbols; Scope *parent; };

typedef struct HostFile HostFile;
struct HostFile {
    uint64_t address;
    FILE *stream;
    int standard, closed;
    HostFile *next;
};

struct CtInterpreter {
    Scope globals;
    Scope *scope;
    Unit *units;
    Memory memory;
    Preprocessor preprocessor;
    const char *filename;
    CtError *error;
    const volatile sig_atomic_t *interrupt;
    FILE *input, *output, *errors;
    unsigned depth, depth_limit;
    size_t steps, step_limit;
    int failed, exit_requested, exit_status, error_number;
    unsigned random_state;
    HostFile *files;
};

CtValue runtime_error(CtInterpreter *interpreter, Token token, const char *message);
CtValue runtime_convert(CtInterpreter *interpreter, Token token, CtValue value, CtType type);
int builtin_type(Token name, CtType *type);
int builtin_value(CtInterpreter *interpreter, Token name, CtValue *value);
void builtin_cleanup(CtInterpreter *interpreter);
CtValue builtin_call(CtInterpreter *interpreter, Token name, const CtValue *args, size_t count);

#endif
