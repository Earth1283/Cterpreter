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

/* The private locals of one activation: scalars no pointer can reach and only
 * lexically bound names use. A symbol here belongs to one declaration for good,
 * so declaring it again only resets its contents. */
typedef struct Frame Frame;
struct Frame {
    Frame *next, *all_next;
    Symbol symbols[];
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

typedef enum { HANDLE_INTERPRETER, HANDLE_TERMINAL } HandleKind;

/* A native object an interpreted program holds by address: an interpreter it created, or a terminal. */
typedef struct Handle Handle;
struct Handle {
    uint64_t address;
    HandleKind kind;
    void *object;
    char *text; /* the native copy of a string the object keeps a pointer to */
    Handle *next;
};

#define BRIDGE_STRINGS 64
#define SIGNAL_SLOTS 32

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
    Handle *handles;
    struct { const char *host; uint64_t address; } bridge_strings[BRIDGE_STRINGS];
    size_t bridge_string_count;
    CtValue result_slot;      /* where a native function writes an aggregate it returns */
    CtValue signal_handlers[SIGNAL_SLOTS];
    void *previous_actions[SIGNAL_SLOTS];
    unsigned char signal_installed[SIGNAL_SLOTS];
    int handlers_installed;   /* whether interpreted code has taken over any signal */
    FunctionRef *functions; /* sorted by address, since addresses are handed out in increasing order */
    size_t function_count, function_capacity;
    VaFrame *va_frame;
    Symbol *symbol_pool;       /* recycled Symbol nodes, avoids malloc/free per scope entry */
    Frame *frame;              /* the running function's private locals */
    Frame *frames;             /* every frame ever made, for ct_clear */
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
void optimize_function_scopes(CtInterpreter *interpreter, Node *function);
int runtime_scalar_call(CtInterpreter *interpreter, Node *call);
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
char *builtin_string(CtInterpreter *interpreter, Token name, CtValue value);
FILE *builtin_stream(CtInterpreter *interpreter, Token name, CtValue value);
CtValue builtin_copy_string(CtInterpreter *interpreter, Token name, const char *text);

/* Native services the front end is built from, callable from interpreted source that declares them. */
enum {
    BR_CT_CREATE, BR_CT_DESTROY, BR_CT_CLEAR, BR_CT_SET_INTERRUPT, BR_CT_SET_FILENAME, BR_CT_SET_LIMITS,
    BR_CT_SET_NESTING, BR_CT_SET_STRICT, BR_CT_EXIT_STATUS, BR_CT_HAS_FUNCTION, BR_CT_RUN_MAIN,
    BR_CT_INSPECT_TYPE, BR_CT_DUMP_AST, BR_CT_DUMP, BR_CT_CHECK, BR_CT_SIGNATURE, BR_CT_COMPLETE,
    BR_CT_TYPE_NAME, BR_CT_TYPE_SIZE, BR_CT_EVAL, BR_CT_FORMAT_VALUE, BR_CT_PRINT_VALUE, BR_CT_DEPTH,
    BR_BOOT_VERIFY, BR_CONFIG_DEFAULTS, BR_CONFIG_INDEX, BR_CONFIG_NAME, BR_CONFIG_VALUE,
    BR_CONFIG_DESCRIPTION, BR_CONFIG_SET, BR_CONFIG_LOAD, BR_CONFIG_SAVE, BR_TERMINAL_INIT,
    BR_TERMINAL_READ, BR_TERMINAL_DESTROY, BR_COUNT
};
CtValue bridge_call(CtInterpreter *interpreter, Token name, size_t id, const CtValue *args);
void bridge_cleanup(CtInterpreter *interpreter);
int bridge_sigaction(CtInterpreter *interpreter, Token name, const CtValue *args);
/* Runs the interpreted handlers of signals that arrived since the last safe point. */
void bridge_deliver(CtInterpreter *interpreter);
extern volatile sig_atomic_t bridge_pending;
CtValue builtin_call(CtInterpreter *interpreter, Token name, size_t id,
                     const CtValue *args, size_t count);

#endif
