#ifndef CT_RUNTIME_H
#define CT_RUNTIME_H

#include "parser.h"
#include "memory.h"
#include "preprocessor.h"

typedef struct Symbol Symbol;
struct Symbol {
    char *name;
    size_t length;
    CtValue value;
    uint64_t address;
    int is_static, is_const, enum_constant;
    Node *function;
    Symbol *next;
};

typedef struct Temporary Temporary;
struct Temporary { uint64_t address; Temporary *next; };

typedef struct Scope Scope;
struct Scope { Symbol *symbols; Temporary *temporaries; Scope *parent; };

typedef struct HostFile HostFile;
struct HostFile {
    uint64_t address;
    FILE *stream;
    int standard, closed;
    HostFile *next;
};

/* Interpreted functions are reachable through pointers by their object address. */
typedef struct FunctionRef FunctionRef;
struct FunctionRef {
    uint64_t address;
    Node *definition;
    FunctionRef *next;
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
    unsigned depth, depth_limit, nesting, nesting_limit;
    size_t steps, step_limit;
    int failed, exit_requested, exit_status, strict;
    uint64_t error_number, token_state;
    unsigned random_state;
    HostFile *files;
    FunctionRef *functions;
};

CtValue runtime_error(CtInterpreter *interpreter, Token token, const char *message);
CtValue runtime_convert(CtInterpreter *interpreter, Token token, CtValue value, CtType type);
CtValue runtime_invoke(CtInterpreter *interpreter, Token name, CtValue pointer, const CtValue *values, size_t count);
int builtin_type(Token name, CtType *type);
size_t builtin_count(void);
const char *builtin_name(size_t index);
int builtin_prototype(Token name, char *buffer, size_t capacity);
int builtin_value(CtInterpreter *interpreter, Token name, CtValue *value);
/* errno is a real object so that a program may assign to it. */
int builtin_object(CtInterpreter *interpreter, Token name, uint64_t *address, CtType *type);
void builtin_cleanup(CtInterpreter *interpreter);
CtValue builtin_call(CtInterpreter *interpreter, Token name, const CtValue *args, size_t count);

#endif
