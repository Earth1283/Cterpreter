#ifndef CT_RUNTIME_H
#define CT_RUNTIME_H

#include "parser.h"
#include "memory.h"
#include "preprocessor.h"

typedef struct Symbol Symbol;
struct Symbol {
    const char *name;    /* a local's name stays in its unit's source; a global's is copied */
    char *owned_name;
    size_t length, name_capacity;
    uint64_t hash;
    CtValue value;
    uint64_t address;
    int is_static, is_const, enum_constant, has_initializer;
    Node *function;
    Allocation *object; /* the record behind address; alive for as long as the symbol */
    Allocation own;     /* the record of an unaddressed local, which has address zero */
    uint64_t scope_serial;
    Node *declaration;        /* local declarations only; global ones may outlive their unit */
    Symbol *shadowed_binding; /* the declaration's binding to restore when this scope ends */
    uint64_t shadowed_scope;
    Symbol *next;        /* declaration-order chain: full-scope iteration and teardown */
    Symbol *bucket_next; /* hash-bucket chain within the owning scope's index, see lookup() */
};

typedef struct Temporary Temporary;
struct Temporary { uint64_t address; Temporary *next; };

typedef struct Scope Scope;
struct Scope {
    Symbol *symbols;
    Symbol **buckets;
    size_t bucket_count, symbol_count;
    uint64_t serial;
    uint64_t name_filter[2]; /* two bits per declared name; a clear bit proves absence */
    Temporary *temporaries;
    Scope *parent;
};

typedef struct HostFile HostFile;
struct HostFile {
    uint64_t address;
    FILE *stream;
    int standard, closed;
    HostFile *next;
};

/* Interpreted functions are reachable through pointers by their object address. */
typedef struct {
    uint64_t address;
    Node *definition;
} FunctionRef;

/* These frames live on the host stack; va_list stores only a managed identity. */
typedef struct VaFrame VaFrame;
struct VaFrame {
    uint64_t identity;
    Node *function;
    Scope *scope;
    CtValue *values;
    size_t count;
    VaFrame *parent;
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
    size_t tick_boundary; /* the next step count that needs the slow checks; 0 once stopped */
    int failed, exit_requested, exit_status, strict;
    uint64_t error_number, token_state;
    unsigned random_state;
    HostFile *files;
    FunctionRef *functions; /* sorted by address, since addresses are handed out in increasing order */
    size_t function_count, function_capacity;
    VaFrame *va_frame;
    Symbol *symbol_pool;       /* recycled Symbol nodes, avoids malloc/free per scope entry */
    Temporary *temporary_pool; /* recycled Temporary nodes, same reason */
    uint64_t scope_serial;
    CtValue returned;         /* the value of the return statement being unwound */
    Node *jump;               /* the goto being unwound */
    CtValue shown;            /* a final unterminated expression's value, for the REPL */
    int showing;
};

CtValue runtime_error(CtInterpreter *interpreter, Token token, const char *message);
CtValue runtime_convert(CtInterpreter *interpreter, Token token, CtValue value, CtType type);
/* Evaluate a side-effect-free node now; fails, leaving no trace, if evaluation would. */
int runtime_fold(CtInterpreter *interpreter, Node *node, CtValue *value);
void optimize_unit(CtInterpreter *interpreter, Unit *unit);
void runtime_prepare(Node *node);
CtValue runtime_invoke(CtInterpreter *interpreter, Token name, CtValue pointer, const CtValue *values, size_t count);
/* Borrow the remaining promoted arguments, consuming the list for v* I/O. */
int runtime_va_values(CtInterpreter *interpreter, Token name, CtValue list, const CtValue **values, size_t *count);
int builtin_type(Token name, CtType *type);
int builtin_resolve(Token name, size_t *id);
size_t builtin_count(void);
const char *builtin_name(size_t index);
int builtin_prototype(Token name, char *buffer, size_t capacity);
int builtin_value(CtInterpreter *interpreter, Token name, CtValue *value);
/* errno is a real object so that a program may assign to it. */
int builtin_object(CtInterpreter *interpreter, Token name, uint64_t *address, CtType *type);
void builtin_cleanup(CtInterpreter *interpreter);
CtValue builtin_call(CtInterpreter *interpreter, Token name, size_t id,
                     const CtValue *args, size_t count);

#endif
