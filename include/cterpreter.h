#ifndef CTERPRETER_H
#define CTERPRETER_H

#include <stddef.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>

#define CT_VERSION "0.3.0"
#define CT_SOURCE_LIMIT (1024u * 1024u)

typedef struct CtInterpreter CtInterpreter;
typedef int CtType;
enum {
    CT_BOOL, CT_CHAR, CT_SCHAR, CT_UCHAR, CT_SHORT, CT_USHORT, CT_INT, CT_UINT,
    CT_LONG, CT_ULONG, CT_LLONG, CT_ULLONG, CT_FLOAT, CT_DOUBLE, CT_VOID
};
typedef struct {
    CtType type;
    union { int64_t integer; uint64_t unsigned_integer; double real; uint64_t address; } as;
} CtValue;
typedef struct {
    size_t line;
    size_t column;
    char message[192];
} CtError;
typedef enum { CT_OK, CT_INCOMPLETE, CT_ERROR } CtStatus;

CtInterpreter *ct_create(void);
void ct_destroy(CtInterpreter *interpreter);
void ct_clear(CtInterpreter *interpreter);
void ct_set_interrupt(CtInterpreter *interpreter, const volatile sig_atomic_t *flag);
void ct_set_filename(CtInterpreter *interpreter, const char *filename);
void ct_set_streams(CtInterpreter *interpreter, FILE *input, FILE *output, FILE *errors);
void ct_set_limits(CtInterpreter *interpreter, size_t steps, unsigned depth);
int ct_exit_status(CtInterpreter *interpreter, int *status);
int ct_has_function(CtInterpreter *interpreter, const char *name);
CtStatus ct_run_main(CtInterpreter *interpreter, int argc, const char *const *argv,
                     int *exit_status, CtError *error);
CtStatus ct_inspect_type(CtInterpreter *interpreter, const char *source, CtType *type, CtError *error);
CtStatus ct_dump_ast(CtInterpreter *interpreter, const char *source, FILE *output, CtError *error);
void ct_dump(CtInterpreter *interpreter, FILE *output);
/* Parse the source without executing it, for diagnostics while typing. */
CtStatus ct_check(CtInterpreter *interpreter, const char *source, CtError *error);
/* Write the declaration of a session function or library function. */
int ct_signature(CtInterpreter *interpreter, const char *name, size_t length, char *buffer, size_t capacity);
/* Write the index-th known name starting with the prefix. */
int ct_complete(CtInterpreter *interpreter, const char *prefix, size_t length, size_t index,
                char *buffer, size_t capacity);
void ct_type_name(CtType type, char *buffer, size_t capacity);
size_t ct_type_size(CtType type);
/* Parse the entire submission before executing. Incomplete/invalid syntax does
 * not change state. Runtime errors preserve earlier completed side effects.
 * A result is produced when the last statement is an expression. */
CtStatus ct_eval(CtInterpreter *interpreter, const char *source,
                 CtValue *result, int *has_result, CtError *error);
void ct_format_value(CtValue value, char *buffer, size_t capacity);
/* Like ct_format_value, but able to read interpreter memory to expand aggregates. */
void ct_print_value(CtInterpreter *interpreter, CtValue value, char *buffer, size_t capacity);
void ct_set_strict(CtInterpreter *interpreter, int strict);
/* The interpreter's own recursion depth: how many interpret() calls deep it is. */
unsigned ct_depth(CtInterpreter *interpreter);
void ct_set_nesting(CtInterpreter *interpreter, unsigned level, unsigned limit);

#endif
