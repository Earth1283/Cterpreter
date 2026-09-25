#include "runtime.h"

#include <ctype.h>
#include <stdint.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHAR_POINTER type_pointer(CT_CHAR)
#define VOID_POINTER type_pointer(CT_VOID)
#define INLINE_ARGUMENTS 4

typedef enum { RT_INT, RT_LONG, RT_ULONG, RT_DOUBLE, RT_VOID, RT_CHAR_POINTER, RT_VOID_POINTER } ReturnKind;

static CtValue integer(int value) { return (CtValue){.type = CT_INT, .as.integer = value}; }
static int named(Token token, const char *name) { return strlen(name) == token.length && !memcmp(token.start, name, token.length); }

static CtType return_type(ReturnKind kind) {
    switch (kind) {
        case RT_LONG: return CT_LONG;
        case RT_ULONG: return CT_ULONG;
        case RT_DOUBLE: return CT_DOUBLE;
        case RT_VOID: return CT_VOID;
        case RT_CHAR_POINTER: return type_pointer(CT_CHAR);
        case RT_VOID_POINTER: return type_pointer(CT_VOID);
        default: return CT_INT;
    }
}

#define VARIADIC 255

typedef struct { const char *name; ReturnKind type; unsigned char arity; const char *parameters; } Signature;

enum {
    BI_PRINTF, BI_SPRINTF, BI_SNPRINTF, BI_FPRINTF, BI_SCANF, BI_SSCANF, BI_FSCANF, BI_VPRINTF, BI_VSPRINTF,
    BI_VSNPRINTF, BI_VFPRINTF, BI_VSCANF, BI_VSSCANF, BI_VFSCANF, BI_PUTS, BI_PUTCHAR, BI_GETCHAR,
    BI_PERROR, BI_FOPEN, BI_FCLOSE, BI_FFLUSH, BI_FGETC, BI_FPUTC, BI_FPUTS, BI_FGETS, BI_GETC, BI_PUTC,
    BI_UNGETC, BI_FREAD, BI_FWRITE, BI_FSEEK, BI_FTELL, BI_REWIND, BI_FEOF, BI_FERROR, BI_CLEARERR,
    BI_REMOVE, BI_RENAME, BI_GETENV, BI_STRERROR, BI_MALLOC, BI_CALLOC, BI_REALLOC, BI_FREE, BI_STRLEN,
    BI_STRCMP, BI_STRNCMP, BI_STRCPY, BI_STRNCPY, BI_STRCAT, BI_STRNCAT, BI_STRCHR, BI_STRRCHR, BI_STRSTR,
    BI_STRSPN, BI_STRCSPN, BI_STRPBRK, BI_STRTOK, BI_MEMCPY, BI_MEMMOVE, BI_MEMSET, BI_MEMCMP, BI_MEMCHR,
    BI_ATOI, BI_ATOL, BI_ATOF, BI_STRTOL, BI_STRTOD, BI_ABS, BI_LABS, BI_RAND, BI_SRAND, BI_QSORT,
    BI_BSEARCH, BI_ABORT, BI_EXIT, BI_SQRT, BI_CBRT, BI_POW, BI_HYPOT, BI_SIN, BI_COS, BI_TAN, BI_ASIN,
    BI_ACOS, BI_ATAN, BI_ATAN2, BI_SINH, BI_COSH, BI_TANH, BI_ASINH, BI_ACOSH, BI_ATANH, BI_EXP,
    BI_EXP2, BI_LOG, BI_LOG2, BI_LOG10, BI_LDEXP, BI_FLOOR, BI_CEIL, BI_ROUND, BI_TRUNC, BI_FABS,
    BI_FMOD, BI_FMIN, BI_FMAX, BI_FDIM, BI_COPYSIGN, BI_TIME, BI_DIFFTIME, BI_CLOCK, BI_ISDIGIT,
    BI_ISALPHA, BI_ISALNUM, BI_ISSPACE, BI_ISUPPER, BI_ISLOWER, BI_ISPUNCT, BI_ISPRINT, BI_ISGRAPH,
    BI_ISCNTRL, BI_ISXDIGIT, BI_ISBLANK, BI_TOUPPER, BI_TOLOWER, BI_ASSERT_FAIL, BI_INTERPRET, BI_INTERPRET_DEPTH,
    BI_COUNT
};

static const Signature signatures[] = {
    [BI_PRINTF] = {"printf", RT_INT, VARIADIC, "const char *format, ..."},
    [BI_SPRINTF] = {"sprintf", RT_INT, VARIADIC, "char *buffer, const char *format, ..."},
    [BI_SNPRINTF] = {"snprintf", RT_INT, VARIADIC, "char *buffer, size_t size, const char *format, ..."},
    [BI_FPRINTF] = {"fprintf", RT_INT, VARIADIC, "FILE *stream, const char *format, ..."},
    [BI_SCANF] = {"scanf", RT_INT, VARIADIC, "const char *format, ..."},
    [BI_SSCANF] = {"sscanf", RT_INT, VARIADIC, "const char *text, const char *format, ..."},
    [BI_FSCANF] = {"fscanf", RT_INT, VARIADIC, "FILE *stream, const char *format, ..."},
    [BI_VPRINTF] = {"vprintf", RT_INT, 2, "const char *format, va_list args"},
    [BI_VSPRINTF] = {"vsprintf", RT_INT, 3, "char *buffer, const char *format, va_list args"},
    [BI_VSNPRINTF] = {"vsnprintf", RT_INT, 4, "char *buffer, size_t size, const char *format, va_list args"},
    [BI_VFPRINTF] = {"vfprintf", RT_INT, 3, "FILE *stream, const char *format, va_list args"},
    [BI_VSCANF] = {"vscanf", RT_INT, 2, "const char *format, va_list args"},
    [BI_VSSCANF] = {"vsscanf", RT_INT, 3, "const char *text, const char *format, va_list args"},
    [BI_VFSCANF] = {"vfscanf", RT_INT, 3, "FILE *stream, const char *format, va_list args"},
    [BI_PUTS] = {"puts", RT_INT, 1, "const char *text"},
    [BI_PUTCHAR] = {"putchar", RT_INT, 1, "int character"},
    [BI_GETCHAR] = {"getchar", RT_INT, 0, "void"},
    [BI_PERROR] = {"perror", RT_VOID, 1, "const char *prefix"},
    [BI_FOPEN] = {"fopen", RT_VOID_POINTER, 2, "const char *path, const char *mode"},
    [BI_FCLOSE] = {"fclose", RT_INT, 1, "FILE *stream"},
    [BI_FFLUSH] = {"fflush", RT_INT, 1, "FILE *stream"},
    [BI_FGETC] = {"fgetc", RT_INT, 1, "FILE *stream"},
    [BI_FPUTC] = {"fputc", RT_INT, 2, "int character, FILE *stream"},
    [BI_FPUTS] = {"fputs", RT_INT, 2, "const char *text, FILE *stream"},
    [BI_FGETS] = {"fgets", RT_CHAR_POINTER, 3, "char *buffer, int size, FILE *stream"},
    [BI_GETC] = {"getc", RT_INT, 1, "FILE *stream"},
    [BI_PUTC] = {"putc", RT_INT, 2, "int character, FILE *stream"},
    [BI_UNGETC] = {"ungetc", RT_INT, 2, "int character, FILE *stream"},
    [BI_FREAD] = {"fread", RT_ULONG, 4, "void *buffer, size_t size, size_t count, FILE *stream"},
    [BI_FWRITE] = {"fwrite", RT_ULONG, 4, "const void *buffer, size_t size, size_t count, FILE *stream"},
    [BI_FSEEK] = {"fseek", RT_INT, 3, "FILE *stream, long offset, int origin"},
    [BI_FTELL] = {"ftell", RT_LONG, 1, "FILE *stream"},
    [BI_REWIND] = {"rewind", RT_VOID, 1, "FILE *stream"},
    [BI_FEOF] = {"feof", RT_INT, 1, "FILE *stream"},
    [BI_FERROR] = {"ferror", RT_INT, 1, "FILE *stream"},
    [BI_CLEARERR] = {"clearerr", RT_VOID, 1, "FILE *stream"},
    [BI_REMOVE] = {"remove", RT_INT, 1, "const char *path"},
    [BI_RENAME] = {"rename", RT_INT, 2, "const char *from, const char *to"},
    [BI_GETENV] = {"getenv", RT_CHAR_POINTER, 1, "const char *name"},
    [BI_STRERROR] = {"strerror", RT_CHAR_POINTER, 1, "int code"},
    [BI_MALLOC] = {"malloc", RT_VOID_POINTER, 1, "size_t size"},
    [BI_CALLOC] = {"calloc", RT_VOID_POINTER, 2, "size_t count, size_t size"},
    [BI_REALLOC] = {"realloc", RT_VOID_POINTER, 2, "void *block, size_t size"},
    [BI_FREE] = {"free", RT_VOID, 1, "void *block"},
    [BI_STRLEN] = {"strlen", RT_ULONG, 1, "const char *text"},
    [BI_STRCMP] = {"strcmp", RT_INT, 2, "const char *left, const char *right"},
    [BI_STRNCMP] = {"strncmp", RT_INT, 3, "const char *left, const char *right, size_t count"},
    [BI_STRCPY] = {"strcpy", RT_CHAR_POINTER, 2, "char *target, const char *source"},
    [BI_STRNCPY] = {"strncpy", RT_CHAR_POINTER, 3, "char *target, const char *source, size_t count"},
    [BI_STRCAT] = {"strcat", RT_CHAR_POINTER, 2, "char *target, const char *source"},
    [BI_STRNCAT] = {"strncat", RT_CHAR_POINTER, 3, "char *target, const char *source, size_t count"},
    [BI_STRCHR] = {"strchr", RT_CHAR_POINTER, 2, "const char *text, int character"},
    [BI_STRRCHR] = {"strrchr", RT_CHAR_POINTER, 2, "const char *text, int character"},
    [BI_STRSTR] = {"strstr", RT_CHAR_POINTER, 2, "const char *haystack, const char *needle"},
    [BI_STRSPN] = {"strspn", RT_ULONG, 2, "const char *text, const char *accepted"},
    [BI_STRCSPN] = {"strcspn", RT_ULONG, 2, "const char *text, const char *rejected"},
    [BI_STRPBRK] = {"strpbrk", RT_CHAR_POINTER, 2, "const char *text, const char *accepted"},
    [BI_STRTOK] = {"strtok", RT_CHAR_POINTER, 2, "char *text, const char *separators"},
    [BI_MEMCPY] = {"memcpy", RT_VOID_POINTER, 3, "void *target, const void *source, size_t count"},
    [BI_MEMMOVE] = {"memmove", RT_VOID_POINTER, 3, "void *target, const void *source, size_t count"},
    [BI_MEMSET] = {"memset", RT_VOID_POINTER, 3, "void *target, int byte, size_t count"},
    [BI_MEMCMP] = {"memcmp", RT_INT, 3, "const void *left, const void *right, size_t count"},
    [BI_MEMCHR] = {"memchr", RT_VOID_POINTER, 3, "const void *block, int byte, size_t count"},
    [BI_ATOI] = {"atoi", RT_INT, 1, "const char *text"},
    [BI_ATOL] = {"atol", RT_LONG, 1, "const char *text"},
    [BI_ATOF] = {"atof", RT_DOUBLE, 1, "const char *text"},
    [BI_STRTOL] = {"strtol", RT_LONG, 3, "const char *text, char **end, int base"},
    [BI_STRTOD] = {"strtod", RT_DOUBLE, 2, "const char *text, char **end"},
    [BI_ABS] = {"abs", RT_INT, 1, "int value"},
    [BI_LABS] = {"labs", RT_LONG, 1, "long value"},
    [BI_RAND] = {"rand", RT_INT, 0, "void"},
    [BI_SRAND] = {"srand", RT_VOID, 1, "unsigned seed"},
    [BI_QSORT] = {"qsort", RT_VOID, 4, "void *base, size_t count, size_t size, int (*compare)(const void *, const void *)"},
    [BI_BSEARCH] = {"bsearch", RT_VOID_POINTER, 5, "const void *key, const void *base, size_t count, size_t size, int (*compare)(const void *, const void *)"},
    [BI_ABORT] = {"abort", RT_VOID, 0, "void"},
    [BI_EXIT] = {"exit", RT_VOID, 1, "int status"},
    [BI_SQRT] = {"sqrt", RT_DOUBLE, 1, "double value"},
    [BI_CBRT] = {"cbrt", RT_DOUBLE, 1, "double value"},
    [BI_POW] = {"pow", RT_DOUBLE, 2, "double base, double exponent"},
    [BI_HYPOT] = {"hypot", RT_DOUBLE, 2, "double x, double y"},
    [BI_SIN] = {"sin", RT_DOUBLE, 1, "double radians"},
    [BI_COS] = {"cos", RT_DOUBLE, 1, "double radians"},
    [BI_TAN] = {"tan", RT_DOUBLE, 1, "double radians"},
    [BI_ASIN] = {"asin", RT_DOUBLE, 1, "double value"},
    [BI_ACOS] = {"acos", RT_DOUBLE, 1, "double value"},
    [BI_ATAN] = {"atan", RT_DOUBLE, 1, "double value"},
    [BI_ATAN2] = {"atan2", RT_DOUBLE, 2, "double y, double x"},
    [BI_SINH] = {"sinh", RT_DOUBLE, 1, "double value"},
    [BI_COSH] = {"cosh", RT_DOUBLE, 1, "double value"},
    [BI_TANH] = {"tanh", RT_DOUBLE, 1, "double value"},
    [BI_ASINH] = {"asinh", RT_DOUBLE, 1, "double value"},
    [BI_ACOSH] = {"acosh", RT_DOUBLE, 1, "double value"},
    [BI_ATANH] = {"atanh", RT_DOUBLE, 1, "double value"},
    [BI_EXP] = {"exp", RT_DOUBLE, 1, "double value"},
    [BI_EXP2] = {"exp2", RT_DOUBLE, 1, "double value"},
    [BI_LOG] = {"log", RT_DOUBLE, 1, "double value"},
    [BI_LOG2] = {"log2", RT_DOUBLE, 1, "double value"},
    [BI_LOG10] = {"log10", RT_DOUBLE, 1, "double value"},
    [BI_LDEXP] = {"ldexp", RT_DOUBLE, 2, "double value, int exponent"},
    [BI_FLOOR] = {"floor", RT_DOUBLE, 1, "double value"},
    [BI_CEIL] = {"ceil", RT_DOUBLE, 1, "double value"},
    [BI_ROUND] = {"round", RT_DOUBLE, 1, "double value"},
    [BI_TRUNC] = {"trunc", RT_DOUBLE, 1, "double value"},
    [BI_FABS] = {"fabs", RT_DOUBLE, 1, "double value"},
    [BI_FMOD] = {"fmod", RT_DOUBLE, 2, "double numerator, double denominator"},
    [BI_FMIN] = {"fmin", RT_DOUBLE, 2, "double left, double right"},
    [BI_FMAX] = {"fmax", RT_DOUBLE, 2, "double left, double right"},
    [BI_FDIM] = {"fdim", RT_DOUBLE, 2, "double left, double right"},
    [BI_COPYSIGN] = {"copysign", RT_DOUBLE, 2, "double magnitude, double sign"},
    [BI_TIME] = {"time", RT_LONG, 1, "time_t *destination"},
    [BI_DIFFTIME] = {"difftime", RT_DOUBLE, 2, "time_t later, time_t earlier"},
    [BI_CLOCK] = {"clock", RT_LONG, 0, "void"},
    [BI_ISDIGIT] = {"isdigit", RT_INT, 1, "int character"},
    [BI_ISALPHA] = {"isalpha", RT_INT, 1, "int character"},
    [BI_ISALNUM] = {"isalnum", RT_INT, 1, "int character"},
    [BI_ISSPACE] = {"isspace", RT_INT, 1, "int character"},
    [BI_ISUPPER] = {"isupper", RT_INT, 1, "int character"},
    [BI_ISLOWER] = {"islower", RT_INT, 1, "int character"},
    [BI_ISPUNCT] = {"ispunct", RT_INT, 1, "int character"},
    [BI_ISPRINT] = {"isprint", RT_INT, 1, "int character"},
    [BI_ISGRAPH] = {"isgraph", RT_INT, 1, "int character"},
    [BI_ISCNTRL] = {"iscntrl", RT_INT, 1, "int character"},
    [BI_ISXDIGIT] = {"isxdigit", RT_INT, 1, "int character"},
    [BI_ISBLANK] = {"isblank", RT_INT, 1, "int character"},
    [BI_TOUPPER] = {"toupper", RT_INT, 1, "int character"},
    [BI_TOLOWER] = {"tolower", RT_INT, 1, "int character"},
    [BI_ASSERT_FAIL] = {"__assert_fail", RT_INT, 3, "const char *expression, const char *file, int line"},
    [BI_INTERPRET] = {"interpret", RT_INT, 1, "const char *path"},
    [BI_INTERPRET_DEPTH] = {"interpret_depth", RT_INT, 0, "void"}
};

_Static_assert(BI_COUNT == sizeof signatures / sizeof signatures[0], "builtin ids must match signatures");

size_t builtin_count(void) { return sizeof signatures / sizeof signatures[0]; }

const char *builtin_name(size_t index) {
    return index < builtin_count() ? signatures[index].name : NULL;
}

static int signature_compare(const char *name, Token token) {
    size_t i = 0;
    for (; i < token.length; ++i) {
        unsigned char a = (unsigned char)name[i], b = (unsigned char)token.start[i];
        if (a != b) return a < b ? -1 : 1;
    }
    return name[i] == '\0' ? 0 : 1;
}

static int signature_order_compare(const void *a, const void *b) {
    return strcmp(signatures[*(const int *)a].name, signatures[*(const int *)b].name);
}

static const int *signature_order(void) {
    static int order[sizeof signatures / sizeof signatures[0]];
    static int ready;
    if (!ready) {
        for (size_t i = 0; i < builtin_count(); ++i) order[i] = (int)i;
        qsort(order, builtin_count(), sizeof *order, signature_order_compare);
        ready = 1;
    }
    return order;
}

static const Signature *find_signature(Token name) {
    const int *order = signature_order();
    size_t lo = 0, hi = builtin_count();
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const Signature *row = &signatures[order[mid]];
        int cmp = signature_compare(row->name, name);
        if (cmp == 0) return row;
        if (cmp < 0) lo = mid + 1; else hi = mid;
    }
    return NULL;
}

int builtin_prototype(Token name, char *buffer, size_t capacity) {
    const Signature *row = find_signature(name);
    if (!row) return 0;
    char returns[64];
    ct_type_name(return_type(row->type), returns, sizeof returns);
    (void)snprintf(buffer, capacity, "%s %s(%s)", returns, row->name, row->parameters);
    return 1;
}

int builtin_type(Token name, CtType *type) {
    const Signature *row = find_signature(name);
    if (!row) return 0;
    *type = return_type(row->type);
    return 1;
}

int builtin_resolve(Token name, size_t *id) {
    const Signature *row = find_signature(name);
    if (!row) return 0;
    *id = (size_t)(row - signatures);
    return 1;
}

static int64_t number(CtInterpreter *interpreter, Token name, CtValue value) {
    if (!type_is_integer(value.type)) {
        (void)runtime_error(interpreter, name, "library argument requires an integer");
        return 0;
    }
    return type_is_signed(value.type) ? value.as.integer : (int64_t)value.as.unsigned_integer;
}

/* Reinterprets an integer as the type a conversion specifier implies. */
static int64_t narrow(CtValue value, CtType type, uint64_t *bits) {
    uint64_t masked = value.as.unsigned_integer & type_mask(type);
    *bits = masked;
    if (type_is_signed(type) && masked > (uint64_t)type_maximum(type))
        return (int64_t)(masked - type_mask(type) - 1);
    return (int64_t)masked;
}

static CtValue integer_of(CtType type, int64_t value) {
    CtValue result = {.type = type};
    uint64_t bits = (uint64_t)value & type_mask(type);
    if (type_is_signed(type) && bits > (uint64_t)type_maximum(type))
        result.as.integer = (int64_t)(bits - type_mask(type) - 1);
    else result.as.unsigned_integer = bits;
    return result;
}

static int small(CtInterpreter *interpreter, Token name, CtValue value) {
    int64_t result = number(interpreter, name, value);
    if (result < INT_MIN || result > INT_MAX) {
        (void)runtime_error(interpreter, name, "library argument does not fit in an int");
        return 0;
    }
    return (int)result;
}

static double real_number(CtInterpreter *interpreter, Token name, CtValue value) {
    if (type_is_real(value.type)) return value.as.real;
    if (!type_is_integer(value.type)) {
        (void)runtime_error(interpreter, name, "library argument requires a number");
        return 0.0;
    }
    return type_is_signed(value.type) ? (double)value.as.integer : (double)value.as.unsigned_integer;
}

static uint64_t pointer(CtInterpreter *interpreter, Token name, CtValue value) {
    if (type_is_pointer(value.type)) return value.as.address;
    if (value.type == CT_INT && !value.as.integer) return 0;
    (void)runtime_error(interpreter, name, "library argument requires a pointer");
    return 0;
}

static char *string(CtInterpreter *interpreter, Token name, CtValue value) {
    uint64_t address = pointer(interpreter, name, value);
    if (interpreter->failed) return NULL;
    char *result = memory_string(&interpreter->memory, address);
    if (!result) (void)runtime_error(interpreter, name, interpreter->memory.error);
    return result;
}

static size_t size_argument(CtInterpreter *interpreter, Token name, CtValue value) {
    int64_t size = number(interpreter, name, value);
    if (size < 0 || (uint64_t)size > CT_SOURCE_LIMIT * 64u) {
        (void)runtime_error(interpreter, name, "size is negative or beyond the interpreter's memory limit");
        return 0;
    }
    return (size_t)size;
}

static void *access_memory(CtInterpreter *interpreter, Token name, uint64_t address, size_t size, int write) {
    if (interpreter->failed) return NULL;
    void *result = memory_access(&interpreter->memory, address, size, write);
    if (!result) (void)runtime_error(interpreter, name, interpreter->memory.error);
    return result;
}

static CtValue host_file(CtInterpreter *interpreter, Token name, FILE *stream, int standard) {
    HostFile *file = calloc(1, sizeof *file);
    if (!file) return runtime_error(interpreter, name, "out of memory");
    file->address = memory_allocate(&interpreter->memory, 1, 1, 0);
    if (!file->address) { free(file); return runtime_error(interpreter, name, interpreter->memory.error); }
    file->stream = stream;
    file->standard = standard;
    file->next = interpreter->files;
    interpreter->files = file;
    return (CtValue){.type = VOID_POINTER, .as.address = file->address};
}

static HostFile *find_file(CtInterpreter *interpreter, Token name, CtValue value) {
    uint64_t address = pointer(interpreter, name, value);
    if (interpreter->failed) return NULL;
    for (HostFile *file = interpreter->files; file; file = file->next)
        if (file->address == address && !file->closed) return file;
    (void)runtime_error(interpreter, name, "invalid or closed file handle");
    return NULL;
}

static FILE *file_stream(CtInterpreter *interpreter, HostFile *file) {
    if (file->standard == 1) return interpreter->input;
    if (file->standard == 2) return interpreter->output;
    if (file->standard == 3) return interpreter->errors;
    return file->stream;
}

static CtValue copy_string(CtInterpreter *interpreter, Token name, const char *text) {
    if (!text) return (CtValue){.type = CHAR_POINTER};
    size_t size = strlen(text) + 1;
    uint64_t address = memory_allocate(&interpreter->memory, size, 1, 0);
    if (!address) return runtime_error(interpreter, name, interpreter->memory.error);
    memcpy(memory_access(&interpreter->memory, address, size, 1), text, size);
    memory_find(&interpreter->memory, address)->readonly = 1;
    return (CtValue){.type = CHAR_POINTER, .as.address = address};
}

static uint64_t errno_object(CtInterpreter *interpreter) {
    if (!interpreter->error_number)
        interpreter->error_number = memory_allocate(&interpreter->memory, ct_type_size(CT_INT), 1, 0);
    return interpreter->error_number;
}

int builtin_object(CtInterpreter *interpreter, Token name, uint64_t *address, CtType *type) {
    if (!named(name, "errno")) return 0;
    *address = errno_object(interpreter);
    *type = CT_INT;
    if (!*address) (void)runtime_error(interpreter, name, interpreter->memory.error);
    return 1;
}

static void set_errno(CtInterpreter *interpreter, int value) {
    uint64_t address = errno_object(interpreter);
    if (address) (void)memory_write(&interpreter->memory, address, integer(value));
}

static int get_errno(CtInterpreter *interpreter) {
    uint64_t address = errno_object(interpreter);
    if (!address) return 0;
    CtValue value = memory_read(&interpreter->memory, address, CT_INT);
    return interpreter->memory.error ? 0 : (int)value.as.integer;
}

int builtin_value(CtInterpreter *interpreter, Token name, CtValue *value) {
    int standard = named(name, "stdin") ? 1 : named(name, "stdout") ? 2 : named(name, "stderr") ? 3 : 0;
    if (!standard) return 0;
    for (HostFile *file = interpreter->files; file; file = file->next)
        if (file->standard == standard && !file->closed) {
            *value = (CtValue){.type = VOID_POINTER, .as.address = file->address};
            return 1;
        }
    *value = host_file(interpreter, name, NULL, standard);
    return 1;
}

void builtin_cleanup(CtInterpreter *interpreter) {
    while (interpreter->files) {
        HostFile *file = interpreter->files;
        interpreter->files = file->next;
        if (!file->standard && !file->closed) fclose(file->stream);
        free(file);
    }
}

typedef struct { char *data; size_t size, capacity; } Output;

/* The conversion and its length modifier name the C type the argument stands for. */
static CtType conversion_type(char conversion, char length, int doubled) {
    int is_signed = conversion == 'd' || conversion == 'i';
    switch (length) {
        case 'h': return doubled ? (is_signed ? CT_SCHAR : CT_UCHAR) : (is_signed ? CT_SHORT : CT_USHORT);
        case 'l': return doubled ? (is_signed ? CT_LLONG : CT_ULLONG) : (is_signed ? CT_LONG : CT_ULONG);
        case 'j': return is_signed ? CT_LLONG : CT_ULLONG;
        case 'z': case 't': return is_signed ? CT_LONG : CT_ULONG;
        default: return is_signed ? CT_INT : CT_UINT;
    }
}

static int append(Output *output, const char *data, size_t length) {
    if (length > CT_SOURCE_LIMIT || output->size > CT_SOURCE_LIMIT - length) return 0;
    size_t needed = output->size + length + 1;
    if (needed > output->capacity) {
        size_t capacity = needed < 256 ? 256 : needed * 2;
        char *grown = realloc(output->data, capacity);
        if (!grown) return 0;
        output->data = grown;
        output->capacity = capacity;
    }
    memcpy(output->data + output->size, data, length);
    output->size += length;
    output->data[output->size] = '\0';
    return 1;
}

typedef struct {
    char conversion;
    int integer_conversion;
    const char *text;
    int64_t signed_value;
    uint64_t unsigned_value;
    double real_value;
} Conversion;

static int format_conversion(char *buffer, size_t capacity, const char *spec, const Conversion *value) {
    switch (value->conversion) {
        case 's': return snprintf(buffer, capacity, spec, value->text);
        case 'c': return snprintf(buffer, capacity, spec, (int)value->signed_value);
        case 'd': case 'i': return snprintf(buffer, capacity, spec, (intmax_t)value->signed_value);
        default:
            if (value->integer_conversion) return snprintf(buffer, capacity, spec, (uintmax_t)value->unsigned_value);
            return snprintf(buffer, capacity, spec, value->real_value);
    }
}

static CtValue formatted(CtInterpreter *interpreter, Token name, size_t id, const CtValue *args, size_t count) {
    int bounded = id == BI_SNPRINTF, to_string = bounded || id == BI_SPRINTF;
    int to_file = id == BI_FPRINTF;
    size_t first = bounded ? 2 : to_string || to_file ? 1 : 0;
    FILE *stream = interpreter->output;
    if (to_file) {
        if (!count) return runtime_error(interpreter, name, "missing stream argument");
        HostFile *file = find_file(interpreter, name, args[0]);
        if (!file) return integer(-1);
        stream = file_stream(interpreter, file);
    }
    if (count <= first) return runtime_error(interpreter, name, "missing format argument");
    char *format = string(interpreter, name, args[first]);
    if (!format) return integer(0);
    Output output = {0};
    size_t argument = first + 1;
    for (size_t i = 0; format[i] && !interpreter->failed; ++i) {
        if (format[i] != '%') {
            size_t run = strcspn(format + i, "%");
            if (!append(&output, format + i, run)) (void)runtime_error(interpreter, name, "formatted output limit exceeded");
            i += run - 1;
            continue;
        }
        ++i;
        if (format[i] == '%') {
            if (!append(&output, "%", 1)) (void)runtime_error(interpreter, name, "formatted output limit exceeded");
            continue;
        }
        char spec[128] = "%";
        size_t used = 1;
        while (format[i] && strchr("-+ #0", format[i])) {
            if (used >= 16) break;
            spec[used++] = format[i++];
        }
        int width = 0;
        if (format[i] == '*') {
            if (argument >= count) { (void)runtime_error(interpreter, name, "missing printf width"); break; }
            width = small(interpreter, name, args[argument++]);
            ++i;
            if (width < 0) { spec[used++] = '-'; if (width == INT_MIN) width = INT_MAX; else width = -width; }
        } else while (isdigit((unsigned char)format[i])) {
            if (width > (int)CT_SOURCE_LIMIT / 10) { width = (int)CT_SOURCE_LIMIT + 1; break; }
            width = width * 10 + format[i++] - '0';
        }
        if (width > (int)CT_SOURCE_LIMIT) { (void)runtime_error(interpreter, name, "printf width exceeds output limit"); break; }
        if (width) used += (size_t)snprintf(spec + used, sizeof spec - used, "%d", width);
        if (format[i] == '.') {
            ++i;
            int precision = 0;
            if (format[i] == '*') {
                if (argument >= count) { (void)runtime_error(interpreter, name, "missing printf precision"); break; }
                precision = small(interpreter, name, args[argument++]);
                ++i;
            } else while (isdigit((unsigned char)format[i])) {
                if (precision > (int)CT_SOURCE_LIMIT / 10) { precision = (int)CT_SOURCE_LIMIT + 1; break; }
                precision = precision * 10 + format[i++] - '0';
            }
            if (precision > (int)CT_SOURCE_LIMIT) { (void)runtime_error(interpreter, name, "printf precision exceeds output limit"); break; }
            if (precision >= 0) used += (size_t)snprintf(spec + used, sizeof spec - used, ".%d", precision);
        }
        char length = 0;
        int doubled = 0;
        if (format[i] && strchr("hljztL", format[i])) {
            length = format[i++];
            if ((length == 'h' || length == 'l') && format[i] == length) { doubled = 1; ++i; }
        }
        char conversion = format[i];
        if (!conversion || !strchr("diuoxXcfFeEgGaAspn", conversion)) {
            (void)runtime_error(interpreter, name, "unsupported printf conversion"); break;
        }
        if (argument >= count) { (void)runtime_error(interpreter, name, "not enough printf arguments"); break; }
        CtValue value = args[argument++];
        if (conversion == 'n') {
            if (length) { (void)runtime_error(interpreter, name, "unsupported %n length modifier"); break; }
            uint64_t destination = pointer(interpreter, name, value);
            if (!interpreter->failed && !memory_write(&interpreter->memory, destination, integer((int)output.size)))
                (void)runtime_error(interpreter, name, interpreter->memory.error);
            continue;
        }
        char *text = NULL;
        int64_t signed_value = 0;
        uint64_t unsigned_value = 0;
        char pointer_text[32];
        double real_value = 0;
        uint64_t address = 0;
        int integer_conversion = strchr("diuoxX", conversion) != NULL;
        if (integer_conversion || conversion == 'c') {
            if (!type_is_integer(value.type)) (void)runtime_error(interpreter, name, "conversion requires an integer");
            else signed_value = narrow(value, conversion == 'c' ? CT_INT : conversion_type(conversion, length, doubled),
                                       &unsigned_value);
        } else if (conversion == 's') text = string(interpreter, name, value);
        else if (conversion == 'p') address = pointer(interpreter, name, value);
        else real_value = real_number(interpreter, name, value);
        if (length == 'L' || ((conversion == 's' || conversion == 'c') && length))
            (void)runtime_error(interpreter, name, "wide strings and long double formats are unsupported");
        if (interpreter->failed) break;
        if (integer_conversion) spec[used++] = 'j';
        if (conversion == 'p') {
            (void)snprintf(pointer_text, sizeof pointer_text, "0x%" PRIx64, address);
            text = pointer_text;
            conversion = 's';
        }
        spec[used++] = conversion;
        spec[used] = '\0';
        Conversion argument_value = {conversion, integer_conversion, text, signed_value, unsigned_value, real_value};
        char inline_piece[128];
        int required = format_conversion(inline_piece, sizeof inline_piece, spec, &argument_value);
        if (required < 0 || required > (int)CT_SOURCE_LIMIT) { (void)runtime_error(interpreter, name, "formatted output limit exceeded"); break; }
        char *piece = (size_t)required < sizeof inline_piece ? inline_piece : malloc((size_t)required + 1);
        if (!piece) { (void)runtime_error(interpreter, name, "out of memory"); break; }
        if (piece != inline_piece) (void)format_conversion(piece, (size_t)required + 1, spec, &argument_value);
        if (!append(&output, piece, (size_t)required)) (void)runtime_error(interpreter, name, "formatted output limit exceeded");
        if (piece != inline_piece) free(piece);
    }
    int result = (int)output.size;
    if (!interpreter->failed) {
        if (to_string) {
            uint64_t destination = pointer(interpreter, name, args[0]);
            size_t capacity = bounded ? size_argument(interpreter, name, args[1]) : output.size + 1;
            size_t length = capacity ? (output.size < capacity - 1 ? output.size : capacity - 1) : 0;
            if (capacity && !interpreter->failed) {
                char *target = access_memory(interpreter, name, destination, length + 1, 1);
                if (target) { if (length) memcpy(target, output.data, length); target[length] = '\0'; }
            }
        } else if (output.size && fwrite(output.data, 1, output.size, stream) != output.size) result = -1;
    }
    free(output.data);
    return integer(result);
}

typedef struct {
    FILE *file;
    const char *text;
    size_t position;
} Input;

static int input_get(Input *input) {
    int character = input->text ? (unsigned char)input->text[input->position] : fgetc(input->file);
    if (input->text && !character) return EOF;
    if (character != EOF) ++input->position;
    return character;
}

static void input_unget(Input *input, int character) {
    if (character == EOF) return;
    if (!input->text) (void)ungetc(character, input->file);
    if (input->position) --input->position;
}

static int input_nonspace(Input *input) {
    int character;
    do { character = input_get(input); } while (character != EOF && isspace((unsigned char)character));
    return character;
}

static CtValue scanned(CtInterpreter *interpreter, Token name, size_t id, const CtValue *args, size_t count) {
    int from_string = id == BI_SSCANF, from_file = id == BI_FSCANF;
    size_t format_index = from_string || from_file ? 1 : 0;
    if (count <= format_index) return runtime_error(interpreter, name, "missing input format");
    Input input = {.file = interpreter->input};
    if (from_string) input.text = string(interpreter, name, args[0]);
    if (from_file) {
        HostFile *file = find_file(interpreter, name, args[0]);
        if (!file) return integer(EOF);
        input.file = file_stream(interpreter, file);
    }
    const char *format = string(interpreter, name, args[format_index]);
    if (interpreter->failed) return integer(0);
    size_t argument = format_index + 1;
    int assignments = 0, eof = 0;
    for (size_t i = 0; format[i] && !interpreter->failed; ++i) {
        if (isspace((unsigned char)format[i])) {
            int character = input_nonspace(&input);
            input_unget(&input, character);
            continue;
        }
        if (format[i] != '%' || format[i + 1] == '%') {
            int expected = format[i] == '%' ? format[++i] : format[i];
            int character = input_get(&input);
            if (character != expected) { eof = character == EOF; input_unget(&input, character); break; }
            continue;
        }
        ++i;
        int suppress = format[i] == '*';
        if (suppress) ++i;
        size_t width = 0;
        while (isdigit((unsigned char)format[i])) {
            if (width > CT_SOURCE_LIMIT / 10) return runtime_error(interpreter, name, "scanf width exceeds limit");
            width = width * 10 + (size_t)(format[i++] - '0');
        }
        if (width > CT_SOURCE_LIMIT) return runtime_error(interpreter, name, "scanf width exceeds limit");
        char modifier = 0;
        int doubled = 0;
        if (format[i] && strchr("hljzt", format[i])) {
            modifier = format[i++];
            if ((modifier == 'h' || modifier == 'l') && format[i] == modifier) { doubled = 1; ++i; }
        }
        char conversion = format[i];
        if (!conversion || !strchr("diuoxXfFeEgGscn", conversion)) return runtime_error(interpreter, name, "unsupported scanf conversion");
        int floating = strchr("fFeEgG", conversion) != NULL;
        CtType type;
        if (conversion == 's' || conversion == 'c') {
            if (modifier) return runtime_error(interpreter, name, "wide scanf destinations are unsupported");
            type = CT_CHAR;
        } else if (floating) {
            if (modifier == 'l' && !doubled) type = CT_DOUBLE;
            else if (!modifier) type = CT_FLOAT;
            else return runtime_error(interpreter, name, "scanf supports float and %lf double destinations");
        } else type = conversion_type(conversion == 'i' ? 'd' : conversion, modifier, doubled);
        uint64_t destination = 0;
        if (!suppress) {
            if (argument >= count) return runtime_error(interpreter, name, "not enough scanf destinations");
            if (args[argument].type != type_pointer(type))
                return runtime_error(interpreter, name, "scanf destination type does not match its format");
            destination = args[argument++].as.address;
        }
        if (conversion == 'n') {
            if (!suppress && !memory_write(&interpreter->memory, destination, integer_of(type, (int64_t)input.position)))
                return runtime_error(interpreter, name, interpreter->memory.error);
            continue;
        }
        int character = conversion == 'c' ? input_get(&input) : input_nonspace(&input);
        if (character == EOF) { eof = 1; break; }
        if (conversion == 's' || conversion == 'c') {
            if (!width) width = conversion == 'c' ? 1 : CT_SOURCE_LIMIT;
            size_t capacity = width + (conversion == 's' ? 1u : 0u);
            if (!suppress) {
                Allocation *object = memory_find(&interpreter->memory, destination);
                if (!object || !object->alive) return runtime_error(interpreter, name, "scanf destination is not a live object");
                size_t available = object->size - (size_t)(destination - object->address);
                if (capacity > available) capacity = available;
            }
            char *text = malloc(capacity ? capacity : 1);
            if (!text) return runtime_error(interpreter, name, "out of memory");
            size_t length = 0;
            while (character != EOF && length < width && (conversion == 'c' || !isspace((unsigned char)character))) {
                if (length >= capacity - (conversion == 's' && capacity ? 1u : 0u)) {
                    free(text);
                    return runtime_error(interpreter, name, "scanf string exceeds destination bounds");
                }
                text[length++] = (char)character;
                if (length == width) { character = EOF; break; }
                character = input_get(&input);
            }
            input_unget(&input, character);
            if (conversion == 's') {
                if (!capacity) { free(text); return runtime_error(interpreter, name, "scanf string destination is empty"); }
                text[length] = '\0';
            }
            if (!length) { free(text); break; }
            if (!suppress) {
                size_t bytes = length + (conversion == 's' ? 1u : 0u);
                void *target = access_memory(interpreter, name, destination, bytes, 1);
                if (target) memcpy(target, text, bytes);
                ++assignments;
            }
            free(text);
        } else {
            char text[256];
            size_t length = 0;
            if (!width || width >= sizeof text) width = sizeof text - 1;
            const char *allowed = floating ? "+-0123456789.eEpPxXaAbBcCdDfF" : conversion == 'x' || conversion == 'X' || conversion == 'i' ? "+-0123456789abcdefABCDEFxX" : "+-0123456789";
            while (character != EOF && strchr(allowed, character) && length < width) {
                text[length++] = (char)character;
                if (length == width) { character = EOF; break; }
                character = input_get(&input);
            }
            input_unget(&input, character);
            text[length] = '\0';
            char *end = NULL;
            CtValue value;
            errno = 0;
            if (floating) {
                double parsed = strtod(text, &end);
                value = (CtValue){.type = type, .as.real = type == CT_FLOAT ? (double)(float)parsed : parsed};
            } else {
                int base = conversion == 'i' ? 0 : conversion == 'o' ? 8 : conversion == 'x' || conversion == 'X' ? 16 : 10;
                if (type_is_signed(type)) {
                    long long parsed = strtoll(text, &end, base);
                    if (parsed < type_minimum(type) || parsed > type_maximum(type)) errno = ERANGE;
                    value = integer_of(type, parsed);
                } else {
                    unsigned long long parsed = strtoull(text, &end, base);
                    if (parsed > type_mask(type)) errno = ERANGE;
                    value = integer_of(type, (int64_t)parsed);
                }
            }
            size_t consumed = (size_t)(end - text);
            while (length > consumed) input_unget(&input, (unsigned char)text[--length]);
            if (!consumed) break;
            if (errno == ERANGE || (floating && !isfinite(value.as.real))) return runtime_error(interpreter, name, "scanf numeric result is out of range");
            if (!suppress) {
                if (!memory_write(&interpreter->memory, destination, value)) return runtime_error(interpreter, name, interpreter->memory.error);
                ++assignments;
            }
        }
    }
    return integer(!assignments && eof ? EOF : assignments);
}

static CtValue file_call(CtInterpreter *interpreter, Token name, size_t id, const CtValue *args) {
    errno = 0;
    if (id == BI_FOPEN) {
        char *path = string(interpreter, name, args[0]);
        char *mode = string(interpreter, name, args[1]);
        if (interpreter->failed) return integer(0);
        FILE *stream = fopen(path, mode);
        set_errno(interpreter, errno);
        if (!stream) return (CtValue){.type = VOID_POINTER};
        CtValue value = host_file(interpreter, name, stream, 0);
        if (interpreter->failed) fclose(stream);
        return value;
    }
    if (id == BI_REMOVE || id == BI_RENAME || id == BI_GETENV || id == BI_STRERROR) {
        if (id == BI_STRERROR) {
            int code = small(interpreter, name, args[0]);
            return interpreter->failed ? integer(0) : copy_string(interpreter, name, strerror(code));
        }
        char *path = string(interpreter, name, args[0]);
        char *target = id == BI_RENAME ? string(interpreter, name, args[1]) : NULL;
        if (interpreter->failed) return integer(0);
        if (id == BI_GETENV) return copy_string(interpreter, name, getenv(path));
        int result = target ? rename(path, target) : remove(path);
        set_errno(interpreter, errno);
        return integer(result);
    }
    size_t stream_index = id == BI_FPUTC || id == BI_FPUTS || id == BI_PUTC ||
                          id == BI_UNGETC ? 1 : id == BI_FGETS ? 2 :
                          id == BI_FREAD || id == BI_FWRITE ? 3 : 0;
    if (id == BI_FFLUSH && ((args[0].type == CT_INT && args[0].as.integer == 0) ||
        (type_is_pointer(args[0].type) && args[0].as.address == 0))) return integer(fflush(NULL));
    HostFile *file = find_file(interpreter, name, args[stream_index]);
    if (!file) return integer(EOF);
    FILE *stream = file_stream(interpreter, file);
    int result = 0;
    if (id == BI_FCLOSE) {
        if (file->standard) return runtime_error(interpreter, name, "closing interpreter-owned standard streams is unsupported");
        result = fclose(stream);
        file->closed = 1;
        (void)memory_release(&interpreter->memory, file->address, 0);
    } else if (id == BI_FFLUSH) result = fflush(stream);
    else if (id == BI_FGETC || id == BI_GETC) result = fgetc(stream);
    else if (id == BI_UNGETC) {
        int character = small(interpreter, name, args[0]);
        if (!interpreter->failed) result = ungetc(character, stream);
    } else if (id == BI_FPUTC || id == BI_PUTC) {
        int character = small(interpreter, name, args[0]);
        if (!interpreter->failed) result = fputc(character, stream);
    } else if (id == BI_FPUTS) {
        char *text = string(interpreter, name, args[0]);
        if (text) result = fputs(text, stream);
    } else if (id == BI_FGETS) {
        uint64_t address = pointer(interpreter, name, args[0]);
        int size = small(interpreter, name, args[1]);
        if (size <= 0) return runtime_error(interpreter, name, "fgets size must be positive");
        char *target = access_memory(interpreter, name, address, (size_t)size, 2);
        if (!target) return integer(0);
        char *read = fgets(target, size, stream);
        if (read) (void)memory_access(&interpreter->memory, address, strlen(read) + 1, 1);
        set_errno(interpreter, errno);
        return (CtValue){.type = CHAR_POINTER, .as.address = read ? address : 0};
    } else if (id == BI_FREAD || id == BI_FWRITE) {
        uint64_t address = pointer(interpreter, name, args[0]);
        size_t size = size_argument(interpreter, name, args[1]);
        size_t elements = size_argument(interpreter, name, args[2]);
        if (size && elements > SIZE_MAX / size) return runtime_error(interpreter, name, "file transfer size overflow");
        if (!size || !elements) return integer(0);
        int reading = id == BI_FREAD;
        void *data = access_memory(interpreter, name, address, size * elements, reading ? 2 : 0);
        if (!data) return integer(0);
        size_t transferred;
        if (reading) {
            size_t bytes = fread(data, 1, size * elements, stream);
            (void)memory_access(&interpreter->memory, address, bytes, 1);
            transferred = bytes / size;
        } else transferred = fwrite(data, size, elements, stream);
        set_errno(interpreter, errno);
        return integer_of(CT_ULONG, (int64_t)transferred);
    } else if (id == BI_FSEEK) {
        int offset = small(interpreter, name, args[1]);
        int origin = small(interpreter, name, args[2]);
        if (origin != SEEK_SET && origin != SEEK_CUR && origin != SEEK_END) return runtime_error(interpreter, name, "invalid seek origin");
        if (!interpreter->failed) result = fseek(stream, offset, origin);
    } else if (id == BI_FTELL) {
        long position = ftell(stream);
        set_errno(interpreter, errno);
        return integer_of(CT_LONG, position);
    } else if (id == BI_REWIND) rewind(stream);
    else if (id == BI_FEOF) result = feof(stream);
    else if (id == BI_FERROR) result = ferror(stream);
    else if (id == BI_CLEARERR) clearerr(stream);
    set_errno(interpreter, errno);
    if (id == BI_REWIND || id == BI_CLEARERR) return (CtValue){.type = CT_VOID};
    return integer(result);
}

/* A nested interpreter shares the caller's streams and its remaining budget. */
static CtValue interpret(CtInterpreter *interpreter, Token name, CtValue argument) {
    char *path = string(interpreter, name, argument);
    if (interpreter->failed) return integer(0);
    if (interpreter->nesting + 1 >= interpreter->nesting_limit)
        return runtime_error(interpreter, name, "nested interpreter limit exceeded");
    FILE *file = fopen(path, "rb");
    if (!file) { set_errno(interpreter, errno); return integer(-1); }
    Output source = {0};
    char buffer[4096];
    size_t bytes;
    int ok = 1;
    while (ok && (bytes = fread(buffer, 1, sizeof buffer, file)) != 0) ok = append(&source, buffer, bytes);
    if (ferror(file)) ok = 0;
    fclose(file);
    if (!ok) { free(source.data); return runtime_error(interpreter, name, "could not read the nested source"); }
    CtInterpreter *child = ct_create();
    if (!child) { free(source.data); return runtime_error(interpreter, name, "out of memory"); }
    size_t budget = interpreter->step_limit > interpreter->steps ? interpreter->step_limit - interpreter->steps : 1;
    /* Nested instances run on the same host stack, so they share one depth budget. */
    unsigned frames = interpreter->depth_limit > interpreter->depth ? interpreter->depth_limit - interpreter->depth : 1;
    ct_set_streams(child, interpreter->input, interpreter->output, interpreter->errors);
    ct_set_interrupt(child, interpreter->interrupt);
    ct_set_limits(child, budget, frames);
    ct_set_nesting(child, interpreter->nesting + 1, interpreter->nesting_limit);
    ct_set_strict(child, interpreter->strict);
    ct_set_filename(child, path);
    CtError diagnostic = {0};
    CtValue result;
    int has_result, status = 0;
    if (ct_eval(child, source.data ? source.data : "", &result, &has_result, &diagnostic) != CT_OK) status = 1;
    else if (ct_exit_status(child, &status)) {}
    else if (ct_has_function(child, "main")) {
        const char *arguments[] = {path};
        if (ct_run_main(child, 1, arguments, &status, &diagnostic) != CT_OK) status = 1;
    }
    if (status == 1 && diagnostic.message[0]) fflush(interpreter->output);
    if (status == 1 && diagnostic.message[0])
        fprintf(interpreter->errors, "%s:%zu:%zu: error: %s\n", path, diagnostic.line, diagnostic.column, diagnostic.message);
    interpreter->steps += budget - (child->step_limit > child->steps ? child->step_limit - child->steps : 0);
    ct_destroy(child);
    free(source.data);
    return integer(status);
}

static CtValue va_formatted(CtInterpreter *interpreter, Token name, size_t id, const CtValue *args, size_t count) {
    const CtValue *remaining = NULL;
    size_t extra = 0, fixed = count - 1;
    if (!runtime_va_values(interpreter, name, args[fixed], &remaining, &extra)) return integer(0);
    size_t total = fixed + extra;
    CtValue inline_values[INLINE_ARGUMENTS];
    CtValue *values = total <= INLINE_ARGUMENTS ? inline_values : malloc(total * sizeof *values);
    if (!values) return runtime_error(interpreter, name, "out of memory");
    memcpy(values, args, fixed * sizeof *values);
    if (extra) memcpy(values + fixed, remaining, extra * sizeof *values);
    /* Reuse the checked format engines; host va_list/host addresses never cross
     * into interpreted memory. Each v* routine sits a fixed distance after its base. */
    size_t base = id - (BI_VPRINTF - BI_PRINTF);
    CtValue result = base >= BI_SCANF ? scanned(interpreter, name, base, values, fixed + extra)
                                      : formatted(interpreter, name, base, values, fixed + extra);
    if (values != inline_values) free(values);
    return result;
}

/* Callbacks may allocate or free, so host pointers are fetched again after each one. */
static int move_bytes(CtInterpreter *interpreter, Token name, uint64_t to, uint64_t from, size_t bytes) {
    void *source = access_memory(interpreter, name, from, bytes, 0);
    void *target = source ? access_memory(interpreter, name, to, bytes, 1) : NULL;
    if (target) memmove(target, source, bytes);
    return target != NULL;
}

static int ordered_after(CtInterpreter *interpreter, Token name, CtValue compare, uint64_t left, uint64_t right) {
    CtValue pair[2] = {{.type = VOID_POINTER, .as.address = left}, {.type = VOID_POINTER, .as.address = right}};
    CtValue order = runtime_invoke(interpreter, name, compare, pair, 2);
    return !interpreter->failed && order.as.integer > 0;
}

/* Bottom-up and stable, like glibc's qsort; the comparator only sees interpreted addresses. */
static void merge_sort(CtInterpreter *interpreter, Token name, uint64_t base, uint64_t scratch,
                       size_t elements, size_t width, CtValue compare) {
    uint64_t from = base, to = scratch;
    for (size_t run = 1; run < elements && !interpreter->failed; run *= 2) {
        for (size_t low = 0; low < elements && !interpreter->failed; low += 2 * run) {
            size_t middle = elements - low > run ? low + run : elements;
            size_t high = elements - middle > run ? middle + run : elements;
            size_t left = low, right = middle, out = low;
            while (left < middle && right < high && !interpreter->failed) {
                int take_right = ordered_after(interpreter, name, compare, from + left * width, from + right * width);
                if (interpreter->failed) break;
                size_t taken = take_right ? right++ : left++;
                if (!move_bytes(interpreter, name, to + out++ * width, from + taken * width, width)) break;
            }
            if (interpreter->failed) break;
            size_t rest = left < middle ? left : right, end = left < middle ? middle : high;
            if (end > rest) (void)move_bytes(interpreter, name, to + out * width, from + rest * width, (end - rest) * width);
        }
        uint64_t swap = from;
        from = to;
        to = swap;
    }
    if (!interpreter->failed && from != base) (void)move_bytes(interpreter, name, base, from, elements * width);
}

static void shell_sort(CtInterpreter *interpreter, Token name, uint64_t base, size_t elements, size_t width, CtValue compare) {
    unsigned char *swap = malloc(width);
    if (!swap) { (void)runtime_error(interpreter, name, "out of memory"); return; }
    for (size_t gap = elements / 2; gap && !interpreter->failed; gap /= 2)
        for (size_t i = gap; i < elements && !interpreter->failed; ++i)
            for (size_t j = i; j >= gap; j -= gap) {
                uint64_t left = base + (j - gap) * width, right = base + j * width;
                if (!ordered_after(interpreter, name, compare, left, right)) break;
                unsigned char *data = access_memory(interpreter, name, base, elements * width, 1);
                if (!data) break;
                memcpy(swap, data + (j - gap) * width, width);
                memcpy(data + (j - gap) * width, data + j * width, width);
                memcpy(data + j * width, swap, width);
            }
    free(swap);
}

CtValue builtin_call(CtInterpreter *interpreter, Token name, size_t id,
                     const CtValue *args, size_t count) {
    const Signature *signature = id < builtin_count() ? &signatures[id] : NULL;
    if (!signature || (signature->arity != VARIADIC && signature->arity != count))
        return runtime_error(interpreter, name, "incorrect number of library arguments");
    if (id >= BI_VPRINTF && id <= BI_VFSCANF) return va_formatted(interpreter, name, id, args, count);
    if (id <= BI_FPRINTF) return formatted(interpreter, name, id, args, count);
    if (id >= BI_SCANF && id <= BI_FSCANF) return scanned(interpreter, name, id, args, count);
    if (id >= BI_FOPEN && id <= BI_STRERROR) return file_call(interpreter, name, id, args);
    if ((id >= BI_STRLEN && id <= BI_STRPBRK) || (id >= BI_ATOI && id <= BI_ATOF)) goto string_functions;
    if (id >= BI_MEMCPY && id <= BI_MEMCHR) goto memory_functions;
    if (id == BI_STRTOL || id == BI_STRTOD) goto number_conversion;
    if (id == BI_STRTOK) goto tokenize;
    if (id == BI_QSORT || id == BI_BSEARCH) goto search_functions;
    if (id >= BI_ISDIGIT && id <= BI_TOLOWER) goto character_functions;
    if (id >= BI_SQRT && id <= BI_COPYSIGN) goto math_functions;
    if (id == BI_GETCHAR) return integer(fgetc(interpreter->input));
    if (id == BI_CLOCK) return integer_of(CT_LONG, (int64_t)clock());
    if (id == BI_RAND) {
        interpreter->random_state = interpreter->random_state * 1103515245u + 12345u;
        return integer((int)((interpreter->random_state / 65536u) % 32768u));
    }
    if (id == BI_ABORT) return runtime_error(interpreter, name, "the program called abort");
    if (id == BI_INTERPRET_DEPTH) return integer((int)interpreter->nesting);
    if (id == BI_INTERPRET) return interpret(interpreter, name, args[0]);
    if (id == BI_PUTS) {
        char *text = string(interpreter, name, args[0]);
        if (!text) return integer(EOF);
        int result = fputs(text, interpreter->output);
        return integer(result < 0 || fputc('\n', interpreter->output) == EOF ? EOF : 0);
    }
    if (id == BI_PUTCHAR) {
        int character = small(interpreter, name, args[0]);
        return integer(interpreter->failed ? EOF : fputc(character, interpreter->output));
    }
    if (id == BI_MALLOC || id == BI_CALLOC || id == BI_REALLOC) {
        size_t size = size_argument(interpreter, name, args[id == BI_REALLOC ? 1 : 0]);
        if (id == BI_CALLOC) {
            size_t elements = size_argument(interpreter, name, args[1]);
            if (elements && size > SIZE_MAX / elements) return runtime_error(interpreter, name, "allocation size overflow");
            size *= elements;
        }
        uint64_t old_address = 0;
        Allocation *old = NULL;
        if (id == BI_REALLOC) {
            old_address = pointer(interpreter, name, args[0]);
            if (old_address) {
                old = memory_find(&interpreter->memory, old_address);
                if (!old || !old->alive || !old->heap || old->address != old_address)
                    return runtime_error(interpreter, name, "realloc requires a live heap allocation");
            }
        }
        if (interpreter->failed) return integer(0);
        uint64_t address = memory_allocate(&interpreter->memory, size, id == BI_CALLOC, 1);
        if (!address) return runtime_error(interpreter, name, interpreter->memory.error);
        if (old) {
            Allocation *target = memory_find(&interpreter->memory, address);
            size_t copied = old->size < size ? old->size : size;
            memcpy(target->data, old->data, copied);
            if (old->fully_initialized) memory_mark(target, 0, copied);
            else {
                memcpy(target->initialized, old->initialized, copied);
                memory_recount(target);
            }
            (void)memory_release(&interpreter->memory, old_address, 1);
        }
        return (CtValue){.type = VOID_POINTER, .as.address = address};
    }
    if (id == BI_FREE) {
        uint64_t address = pointer(interpreter, name, args[0]);
        if (!interpreter->failed && !memory_release(&interpreter->memory, address, 1))
            return runtime_error(interpreter, name, interpreter->memory.error);
        return (CtValue){.type = CT_VOID};
    }
string_functions:
    if (id == BI_STRLEN || id == BI_STRCMP || id == BI_STRNCMP || id == BI_STRCPY ||
        id == BI_STRNCPY || id == BI_STRCAT || id == BI_STRNCAT || id == BI_STRCHR ||
        id == BI_STRRCHR || id == BI_STRSTR || id == BI_STRSPN || id == BI_STRCSPN ||
        id == BI_STRPBRK || id == BI_ATOI || id == BI_ATOL || id == BI_ATOF) {
        int copying = id == BI_STRCPY || id == BI_STRNCPY;
        int character_argument = id == BI_STRCHR || id == BI_STRRCHR;
        char *a = copying ? NULL : string(interpreter, name, args[0]);
        char *b = count > 1 && !character_argument ? string(interpreter, name, args[1]) : NULL;
        if (interpreter->failed) return integer(0);
        if (id == BI_STRLEN) return integer_of(CT_ULONG, (int64_t)strlen(a));
        if (id == BI_ATOI || id == BI_ATOL) {
            errno = 0;
            long value = strtol(a, NULL, 10);
            int wide = id == BI_ATOL;
            if (errno == ERANGE || (!wide && (value < INT_MIN || value > INT_MAX)))
                return runtime_error(interpreter, name, "the parsed value is out of range");
            return integer_of(wide ? CT_LONG : CT_INT, value);
        }
        if (id == BI_ATOF) {
            errno = 0;
            double value = strtod(a, NULL);
            if (errno == ERANGE || !isfinite(value)) return runtime_error(interpreter, name, "atof result out of range");
            return (CtValue){.type = CT_DOUBLE, .as.real = value};
        }
        if (id == BI_STRCMP) return integer(strcmp(a, b));
        if (id == BI_STRNCMP) {
            size_t n = size_argument(interpreter, name, args[2]);
            return interpreter->failed ? integer(0) : integer(strncmp(a, b, n));
        }
        if (id == BI_STRSPN) return integer_of(CT_ULONG, (int64_t)strspn(a, b));
        if (id == BI_STRCSPN) return integer_of(CT_ULONG, (int64_t)strcspn(a, b));
        if (id == BI_STRCHR || id == BI_STRRCHR || id == BI_STRSTR || id == BI_STRPBRK) {
            int character = id == BI_STRSTR || id == BI_STRPBRK ? 0 : small(interpreter, name, args[1]);
            if (interpreter->failed) return integer(0);
            char *found = id == BI_STRCHR ? strchr(a, character)
                        : id == BI_STRRCHR ? strrchr(a, character)
                        : id == BI_STRPBRK ? strpbrk(a, b) : strstr(a, b);
            return (CtValue){.type = CHAR_POINTER, .as.address = found ? args[0].as.address + (uint64_t)(found - a) : 0};
        }
        uint64_t address = pointer(interpreter, name, args[0]);
        int appending = id == BI_STRCAT || id == BI_STRNCAT;
        size_t prefix = appending ? strlen(a) : 0;
        size_t limit = id == BI_STRNCPY || id == BI_STRNCAT ? size_argument(interpreter, name, args[2]) : 0;
        size_t bytes = id == BI_STRNCPY ? limit
                     : id == BI_STRNCAT ? (strlen(b) < limit ? strlen(b) : limit) + 1
                     : strlen(b) + 1;
        char *destination = access_memory(interpreter, name, address + prefix, bytes, 1);
        if (destination) {
            if (id == BI_STRNCPY) {
                size_t copied = strlen(b) < bytes ? strlen(b) : bytes;
                memmove(destination, b, copied);
                memset(destination + copied, 0, bytes - copied);
            } else if (id == BI_STRNCAT) {
                memmove(destination, b, bytes - 1);
                destination[bytes - 1] = '\0';
            } else memmove(destination, b, bytes);
        }
        return (CtValue){.type = CHAR_POINTER, .as.address = address};
    }
memory_functions:
    if (id == BI_MEMCHR) {
        uint64_t address = pointer(interpreter, name, args[0]);
        int character = small(interpreter, name, args[1]);
        size_t bytes = size_argument(interpreter, name, args[2]);
        void *data = access_memory(interpreter, name, address, bytes, 0);
        if (!data) return integer(0);
        unsigned char *found = memchr(data, character, bytes);
        return (CtValue){.type = VOID_POINTER, .as.address = found ? address + (uint64_t)(found - (unsigned char *)data) : 0};
    }
    if (id == BI_MEMCPY || id == BI_MEMMOVE || id == BI_MEMSET || id == BI_MEMCMP) {
        uint64_t destination = pointer(interpreter, name, args[0]);
        size_t bytes = size_argument(interpreter, name, args[2]);
        int setting = id == BI_MEMSET, comparing = id == BI_MEMCMP;
        int character = setting ? small(interpreter, name, args[1]) : 0;
        uint64_t source = setting ? 0 : pointer(interpreter, name, args[1]);
        void *from = setting ? NULL : access_memory(interpreter, name, source, bytes, 0);
        void *to = access_memory(interpreter, name, destination, bytes, !comparing);
        if (interpreter->failed) return integer(0);
        if (comparing) return integer(memcmp(to, from, bytes));
        if (setting) memset(to, character, bytes);
        else memmove(to, from, bytes);
        return (CtValue){.type = VOID_POINTER, .as.address = destination};
    }
    if (id == BI_PERROR) {
        char *prefix = string(interpreter, name, args[0]);
        if (!interpreter->failed)
            fprintf(interpreter->errors, "%s%s%s\n", prefix && *prefix ? prefix : "",
                    prefix && *prefix ? ": " : "", strerror(get_errno(interpreter)));
        return (CtValue){.type = CT_VOID};
    }
    if (id == BI_TIME) {
        time_t now = time(NULL);
        if (now == (time_t)-1) return runtime_error(interpreter, name, "the clock is unavailable");
        CtValue seconds = integer_of(CT_LONG, (int64_t)now);
        uint64_t destination = pointer(interpreter, name, args[0]);
        if (destination && !memory_write(&interpreter->memory, destination, seconds))
            return runtime_error(interpreter, name, interpreter->memory.error);
        return seconds;
    }
    if (id == BI_ASSERT_FAIL) {
        char *expression = string(interpreter, name, args[0]);
        char *file = string(interpreter, name, args[1]);
        int line = small(interpreter, name, args[2]);
        if (interpreter->failed) return integer(0);
        char message[192];
        (void)snprintf(message, sizeof message, "assertion failed: %s (%s:%d)", expression, file, line);
        return runtime_error(interpreter, name, message);
    }
number_conversion:
    if (id == BI_STRTOL || id == BI_STRTOD) {
        char *text = string(interpreter, name, args[0]);
        int base = id == BI_STRTOL ? small(interpreter, name, args[2]) : 0;
        if (interpreter->failed) return integer(0);
        if (id == BI_STRTOL && base != 0 && (base < 2 || base > 36))
            return runtime_error(interpreter, name, "strtol requires a base of 0 or 2 through 36");
        char *end = NULL;
        errno = 0;
        double real_result = 0;
        long integer_result = 0;
        if (id == BI_STRTOL) integer_result = strtol(text, &end, base);
        else real_result = strtod(text, &end);
        set_errno(interpreter, errno);
        uint64_t destination = pointer(interpreter, name, args[1]);
        if (destination) {
            CtValue position = {.type = CHAR_POINTER, .as.address = args[0].as.address + (uint64_t)(end - text)};
            if (!memory_write(&interpreter->memory, destination, position))
                return runtime_error(interpreter, name, interpreter->memory.error);
        }
        if (id == BI_STRTOD) return (CtValue){.type = CT_DOUBLE, .as.real = real_result};
        return integer_of(CT_LONG, integer_result);
    }
tokenize:
    if (id == BI_STRTOK) {
        char *separators = string(interpreter, name, args[1]);
        uint64_t address = args[0].type == CT_INT && !args[0].as.integer ? 0 : pointer(interpreter, name, args[0]);
        if (!address) address = interpreter->token_state;
        if (interpreter->failed || !address) return (CtValue){.type = CHAR_POINTER};
        char *text = memory_string(&interpreter->memory, address);
        if (!text) return runtime_error(interpreter, name, interpreter->memory.error);
        size_t start = strspn(text, separators);
        if (!text[start]) { interpreter->token_state = 0; return (CtValue){.type = CHAR_POINTER}; }
        size_t length = strcspn(text + start, separators);
        if (text[start + length]) {
            char *terminator = access_memory(interpreter, name, address + start + length, 1, 1);
            if (!terminator) return integer(0);
            *terminator = '\0';
            interpreter->token_state = address + start + length + 1;
        } else interpreter->token_state = 0;
        return (CtValue){.type = CHAR_POINTER, .as.address = address + start};
    }
search_functions:
    if (id == BI_QSORT || id == BI_BSEARCH) {
        int searching = id == BI_BSEARCH;
        uint64_t base = pointer(interpreter, name, args[searching]);
        size_t elements = size_argument(interpreter, name, args[searching + 1]);
        size_t width = size_argument(interpreter, name, args[searching + 2]);
        CtValue compare = args[searching + 3];
        if (interpreter->failed) return integer(0);
        if (width && elements > SIZE_MAX / width) return runtime_error(interpreter, name, "array size overflow");
        if (!elements || !width) return searching ? (CtValue){.type = VOID_POINTER} : (CtValue){.type = CT_VOID};
        if (!access_memory(interpreter, name, base, elements * width, 0)) return integer(0);
        unsigned char *data = access_memory(interpreter, name, base, elements * width, 1);
        if (!data) return integer(0);
        CtValue pair[2] = {{.type = VOID_POINTER}, {.type = VOID_POINTER}};
        if (searching) {
            pair[0] = args[0];
            size_t low = 0, high = elements;
            while (low < high && !interpreter->failed) {
                size_t middle = low + (high - low) / 2;
                pair[1] = (CtValue){.type = VOID_POINTER, .as.address = base + middle * width};
                CtValue order = runtime_invoke(interpreter, name, compare, pair, 2);
                if (interpreter->failed) return integer(0);
                if (!order.as.integer) return pair[1];
                if (order.as.integer < 0) high = middle;
                else low = middle + 1;
            }
            return (CtValue){.type = VOID_POINTER};
        }
        uint64_t scratch = memory_allocate(&interpreter->memory, elements * width, 0, 0);
        if (scratch) {
            merge_sort(interpreter, name, base, scratch, elements, width, compare);
            (void)memory_release(&interpreter->memory, scratch, 0);
        } else shell_sort(interpreter, name, base, elements, width, compare);
        return (CtValue){.type = CT_VOID};
    }
    if (id == BI_EXIT || id == BI_SRAND || id == BI_ABS || id == BI_LABS) {
        if (id == BI_LABS) {
            int64_t wide = number(interpreter, name, args[0]);
            if (interpreter->failed) return integer(0);
            if (wide == INT64_MIN) return runtime_error(interpreter, name, "labs result overflows long");
            return integer_of(CT_LONG, wide < 0 ? -wide : wide);
        }
        int value = small(interpreter, name, args[0]);
        if (interpreter->failed) return integer(0);
        if (id == BI_EXIT) { interpreter->exit_requested = 1; interpreter->tick_boundary = 0; interpreter->exit_status = value; return (CtValue){.type = CT_VOID}; }
        if (id == BI_SRAND) { interpreter->random_state = (unsigned)value; return (CtValue){.type = CT_VOID}; }
        if (value == INT_MIN) return runtime_error(interpreter, name, "abs result overflows int");
        return integer_of(id == BI_LABS ? CT_LONG : CT_INT, abs(value));
    }
character_functions:
    if ((id >= BI_ISDIGIT && id <= BI_ISBLANK) || id == BI_TOUPPER || id == BI_TOLOWER) {
        int value = small(interpreter, name, args[0]);
        if (value != EOF && (value < 0 || value > UCHAR_MAX)) return runtime_error(interpreter, name, "ctype requires an unsigned char or EOF");
        if (interpreter->failed) return integer(0);
        if (id == BI_ISDIGIT) return integer(isdigit(value));
        if (id == BI_ISALPHA) return integer(isalpha(value));
        if (id == BI_ISALNUM) return integer(isalnum(value));
        if (id == BI_ISSPACE) return integer(isspace(value));
        if (id == BI_ISUPPER) return integer(isupper(value));
        if (id == BI_ISLOWER) return integer(islower(value));
        if (id == BI_ISPUNCT) return integer(ispunct(value));
        if (id == BI_ISPRINT) return integer(isprint(value));
        if (id == BI_ISGRAPH) return integer(isgraph(value));
        if (id == BI_ISCNTRL) return integer(iscntrl(value));
        if (id == BI_ISXDIGIT) return integer(isxdigit(value));
        if (id == BI_ISBLANK) return integer(value == ' ' || value == '\t');
        return integer(id == BI_TOUPPER ? toupper(value) : tolower(value));
    }
math_functions:
    (void)0;
    double a = real_number(interpreter, name, args[0]);
    double b = count == 2 ? real_number(interpreter, name, args[1]) : 0.0;
    if (interpreter->failed) return integer(0);
    double value = 0;
    errno = 0;
    switch (id) {
        case BI_SQRT: value = sqrt(a); break;
        case BI_CBRT: value = cbrt(a); break;
        case BI_HYPOT: value = hypot(a, b); break;
        case BI_SINH: value = sinh(a); break;
        case BI_COSH: value = cosh(a); break;
        case BI_TANH: value = tanh(a); break;
        case BI_ASINH: value = asinh(a); break;
        case BI_ACOSH: value = acosh(a); break;
        case BI_ATANH: value = atanh(a); break;
        case BI_EXP2: value = exp2(a); break;
        case BI_LOG2: value = log2(a); break;
        case BI_LDEXP: value = ldexp(a, (int)b); break;
        case BI_FMIN: value = fmin(a, b); break;
        case BI_FMAX: value = fmax(a, b); break;
        case BI_FDIM: value = fdim(a, b); break;
        case BI_COPYSIGN: value = copysign(a, b); break;
        case BI_DIFFTIME: value = a - b; break;
        case BI_POW: value = pow(a, b); break;
        case BI_SIN: value = sin(a); break;
        case BI_COS: value = cos(a); break;
        case BI_TAN: value = tan(a); break;
        case BI_ASIN: value = asin(a); break;
        case BI_ACOS: value = acos(a); break;
        case BI_ATAN: value = atan(a); break;
        case BI_ATAN2: value = atan2(a, b); break;
        case BI_EXP: value = exp(a); break;
        case BI_LOG: value = log(a); break;
        case BI_LOG10: value = log10(a); break;
        case BI_FLOOR: value = floor(a); break;
        case BI_CEIL: value = ceil(a); break;
        case BI_ROUND: value = round(a); break;
        case BI_TRUNC: value = trunc(a); break;
        case BI_FABS: value = fabs(a); break;
        case BI_FMOD: value = fmod(a, b); break;
        default: return runtime_error(interpreter, name, "unknown library function");
    }
    if (errno == EDOM || errno == ERANGE || !isfinite(value)) return runtime_error(interpreter, name, "math result is outside its domain or range");
    return (CtValue){.type = CT_DOUBLE, .as.real = value};
}
