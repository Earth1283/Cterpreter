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
enum { CT_INT, CT_DOUBLE, CT_CHAR, CT_VOID };
typedef struct {
    CtType type;
    union { int integer; double real; uint64_t address; } as;
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
unsigned ct_depth(CtInterpreter *interpreter);

#endif
