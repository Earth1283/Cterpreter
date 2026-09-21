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

/* Stable positions in signatures[]. Ranges let the call dispatcher jump
 * straight to a function family after its one binary-search lookup. */
enum {
    BI_PRINTF = 0, BI_FPRINTF = 3,
    BI_SCANF = 4, BI_FSCANF = 6,
    BI_VPRINTF = 7, BI_VFSCANF = 13,
    BI_FOPEN = 18, BI_STRERROR = 39,
    BI_MALLOC = 40, BI_FREE = 43,
    BI_STRLEN = 44, BI_STRPBRK = 56, BI_STRTOK = 57,
    BI_MEMCPY = 58, BI_MEMCHR = 62,
    BI_ATOI = 63, BI_ATOF = 65, BI_STRTOL = 66, BI_STRTOD = 67,
    BI_QSORT = 72, BI_BSEARCH = 73,
    BI_SQRT = 76, BI_COPYSIGN = 108,
    BI_ISDIGIT = 112, BI_TOLOWER = 125,
    BI_COUNT = 129
};

static const Signature signatures[] = {
    {"printf", RT_INT, VARIADIC, "const char *format, ..."},
    {"sprintf", RT_INT, VARIADIC, "char *buffer, const char *format, ..."},
    {"snprintf", RT_INT, VARIADIC, "char *buffer, size_t size, const char *format, ..."},
    {"fprintf", RT_INT, VARIADIC, "FILE *stream, const char *format, ..."},
    {"scanf", RT_INT, VARIADIC, "const char *format, ..."},
    {"sscanf", RT_INT, VARIADIC, "const char *text, const char *format, ..."},
    {"fscanf", RT_INT, VARIADIC, "FILE *stream, const char *format, ..."},
    {"vprintf", RT_INT, 2, "const char *format, va_list args"},
    {"vsprintf", RT_INT, 3, "char *buffer, const char *format, va_list args"},
    {"vsnprintf", RT_INT, 4, "char *buffer, size_t size, const char *format, va_list args"},
    {"vfprintf", RT_INT, 3, "FILE *stream, const char *format, va_list args"},
    {"vscanf", RT_INT, 2, "const char *format, va_list args"},
    {"vsscanf", RT_INT, 3, "const char *text, const char *format, va_list args"},
    {"vfscanf", RT_INT, 3, "FILE *stream, const char *format, va_list args"},
    {"puts", RT_INT, 1, "const char *text"},
    {"putchar", RT_INT, 1, "int character"},
    {"getchar", RT_INT, 0, "void"},
    {"perror", RT_VOID, 1, "const char *prefix"},
    {"fopen", RT_VOID_POINTER, 2, "const char *path, const char *mode"},
    {"fclose", RT_INT, 1, "FILE *stream"},
    {"fflush", RT_INT, 1, "FILE *stream"},
    {"fgetc", RT_INT, 1, "FILE *stream"},
    {"fputc", RT_INT, 2, "int character, FILE *stream"},
    {"fputs", RT_INT, 2, "const char *text, FILE *stream"},
    {"fgets", RT_CHAR_POINTER, 3, "char *buffer, int size, FILE *stream"},
    {"getc", RT_INT, 1, "FILE *stream"},
    {"putc", RT_INT, 2, "int character, FILE *stream"},
    {"ungetc", RT_INT, 2, "int character, FILE *stream"},
    {"fread", RT_ULONG, 4, "void *buffer, size_t size, size_t count, FILE *stream"},
    {"fwrite", RT_ULONG, 4, "const void *buffer, size_t size, size_t count, FILE *stream"},
    {"fseek", RT_INT, 3, "FILE *stream, long offset, int origin"},
    {"ftell", RT_LONG, 1, "FILE *stream"},
    {"rewind", RT_VOID, 1, "FILE *stream"},
    {"feof", RT_INT, 1, "FILE *stream"},
    {"ferror", RT_INT, 1, "FILE *stream"},
    {"clearerr", RT_VOID, 1, "FILE *stream"},
    {"remove", RT_INT, 1, "const char *path"},
    {"rename", RT_INT, 2, "const char *from, const char *to"},
    {"getenv", RT_CHAR_POINTER, 1, "const char *name"},
    {"strerror", RT_CHAR_POINTER, 1, "int code"},
    {"malloc", RT_VOID_POINTER, 1, "size_t size"},
    {"calloc", RT_VOID_POINTER, 2, "size_t count, size_t size"},
    {"realloc", RT_VOID_POINTER, 2, "void *block, size_t size"},
    {"free", RT_VOID, 1, "void *block"},
    {"strlen", RT_ULONG, 1, "const char *text"},
    {"strcmp", RT_INT, 2, "const char *left, const char *right"},
    {"strncmp", RT_INT, 3, "const char *left, const char *right, size_t count"},
    {"strcpy", RT_CHAR_POINTER, 2, "char *target, const char *source"},
    {"strncpy", RT_CHAR_POINTER, 3, "char *target, const char *source, size_t count"},
    {"strcat", RT_CHAR_POINTER, 2, "char *target, const char *source"},
    {"strncat", RT_CHAR_POINTER, 3, "char *target, const char *source, size_t count"},
    {"strchr", RT_CHAR_POINTER, 2, "const char *text, int character"},
    {"strrchr", RT_CHAR_POINTER, 2, "const char *text, int character"},
    {"strstr", RT_CHAR_POINTER, 2, "const char *haystack, const char *needle"},
    {"strspn", RT_ULONG, 2, "const char *text, const char *accepted"},
    {"strcspn", RT_ULONG, 2, "const char *text, const char *rejected"},
    {"strpbrk", RT_CHAR_POINTER, 2, "const char *text, const char *accepted"},
    {"strtok", RT_CHAR_POINTER, 2, "char *text, const char *separators"},
    {"memcpy", RT_VOID_POINTER, 3, "void *target, const void *source, size_t count"},
    {"memmove", RT_VOID_POINTER, 3, "void *target, const void *source, size_t count"},
    {"memset", RT_VOID_POINTER, 3, "void *target, int byte, size_t count"},
    {"memcmp", RT_INT, 3, "const void *left, const void *right, size_t count"},
    {"memchr", RT_VOID_POINTER, 3, "const void *block, int byte, size_t count"},
    {"atoi", RT_INT, 1, "const char *text"},
    {"atol", RT_LONG, 1, "const char *text"},
    {"atof", RT_DOUBLE, 1, "const char *text"},
    {"strtol", RT_LONG, 3, "const char *text, char **end, int base"},
    {"strtod", RT_DOUBLE, 2, "const char *text, char **end"},
    {"abs", RT_INT, 1, "int value"},
    {"labs", RT_LONG, 1, "long value"},
    {"rand", RT_INT, 0, "void"},
    {"srand", RT_VOID, 1, "unsigned seed"},
    {"qsort", RT_VOID, 4, "void *base, size_t count, size_t size, int (*compare)(const void *, const void *)"},
    {"bsearch", RT_VOID_POINTER, 5, "const void *key, const void *base, size_t count, size_t size, int (*compare)(const void *, const void *)"},
    {"abort", RT_VOID, 0, "void"},
    {"exit", RT_VOID, 1, "int status"},
    {"sqrt", RT_DOUBLE, 1, "double value"},
    {"cbrt", RT_DOUBLE, 1, "double value"},
    {"pow", RT_DOUBLE, 2, "double base, double exponent"},
    {"hypot", RT_DOUBLE, 2, "double x, double y"},
    {"sin", RT_DOUBLE, 1, "double radians"},
    {"cos", RT_DOUBLE, 1, "double radians"},
    {"tan", RT_DOUBLE, 1, "double radians"},
    {"asin", RT_DOUBLE, 1, "double value"},
    {"acos", RT_DOUBLE, 1, "double value"},
    {"atan", RT_DOUBLE, 1, "double value"},
    {"atan2", RT_DOUBLE, 2, "double y, double x"},
    {"sinh", RT_DOUBLE, 1, "double value"},
    {"cosh", RT_DOUBLE, 1, "double value"},
    {"tanh", RT_DOUBLE, 1, "double value"},
    {"asinh", RT_DOUBLE, 1, "double value"},
    {"acosh", RT_DOUBLE, 1, "double value"},
    {"atanh", RT_DOUBLE, 1, "double value"},
    {"exp", RT_DOUBLE, 1, "double value"},
    {"exp2", RT_DOUBLE, 1, "double value"},
    {"log", RT_DOUBLE, 1, "double value"},
    {"log2", RT_DOUBLE, 1, "double value"},
    {"log10", RT_DOUBLE, 1, "double value"},
    {"ldexp", RT_DOUBLE, 2, "double value, int exponent"},
    {"floor", RT_DOUBLE, 1, "double value"},
    {"ceil", RT_DOUBLE, 1, "double value"},
    {"round", RT_DOUBLE, 1, "double value"},
    {"trunc", RT_DOUBLE, 1, "double value"},
    {"fabs", RT_DOUBLE, 1, "double value"},
    {"fmod", RT_DOUBLE, 2, "double numerator, double denominator"},
    {"fmin", RT_DOUBLE, 2, "double left, double right"},
    {"fmax", RT_DOUBLE, 2, "double left, double right"},
    {"fdim", RT_DOUBLE, 2, "double left, double right"},
    {"copysign", RT_DOUBLE, 2, "double magnitude, double sign"},
    {"time", RT_LONG, 1, "time_t *destination"},
    {"difftime", RT_DOUBLE, 2, "time_t later, time_t earlier"},
    {"clock", RT_LONG, 0, "void"},
    {"isdigit", RT_INT, 1, "int character"},
    {"isalpha", RT_INT, 1, "int character"},
    {"isalnum", RT_INT, 1, "int character"},
    {"isspace", RT_INT, 1, "int character"},
    {"isupper", RT_INT, 1, "int character"},
    {"islower", RT_INT, 1, "int character"},
    {"ispunct", RT_INT, 1, "int character"},
    {"isprint", RT_INT, 1, "int character"},
    {"isgraph", RT_INT, 1, "int character"},
    {"iscntrl", RT_INT, 1, "int character"},
    {"isxdigit", RT_INT, 1, "int character"},
    {"isblank", RT_INT, 1, "int character"},
    {"toupper", RT_INT, 1, "int character"},
    {"tolower", RT_INT, 1, "int character"},
    {"__assert_fail", RT_INT, 3, "const char *expression, const char *file, int line"},
    {"interpret", RT_INT, 1, "const char *path"},
    {"interpret_depth", RT_INT, 0, "void"}
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

static int require_count(CtInterpreter *interpreter, Token name, size_t count) {
    const Signature *row = find_signature(name);
    if (row && (row->arity == VARIADIC || row->arity == count)) return 1;
    (void)runtime_error(interpreter, name, "incorrect number of library arguments");
    return 0;
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

static CtValue formatted(CtInterpreter *interpreter, Token name, const CtValue *args, size_t count) {
    int bounded = named(name, "snprintf"), to_string = bounded || named(name, "sprintf");
    int to_file = named(name, "fprintf");
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
            if (!append(&output, format + i, 1)) (void)runtime_error(interpreter, name, "formatted output limit exceeded");
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
        int required;
        if (conversion == 's') required = snprintf(NULL, 0, spec, text);
        else if (conversion == 'c') required = snprintf(NULL, 0, spec, (int)signed_value);
        else if (conversion == 'd' || conversion == 'i') required = snprintf(NULL, 0, spec, (intmax_t)signed_value);
        else if (integer_conversion) required = snprintf(NULL, 0, spec, (uintmax_t)unsigned_value);
        else required = snprintf(NULL, 0, spec, real_value);
        if (required < 0 || required > (int)CT_SOURCE_LIMIT) { (void)runtime_error(interpreter, name, "formatted output limit exceeded"); break; }
        char inline_piece[128];
        char *piece = (size_t)required < sizeof inline_piece
            ? inline_piece : malloc((size_t)required + 1);
        if (!piece) { (void)runtime_error(interpreter, name, "out of memory"); break; }
        if (conversion == 's') (void)snprintf(piece, (size_t)required + 1, spec, text);
        else if (conversion == 'c') (void)snprintf(piece, (size_t)required + 1, spec, (int)signed_value);
        else if (conversion == 'd' || conversion == 'i') (void)snprintf(piece, (size_t)required + 1, spec, (intmax_t)signed_value);
        else if (integer_conversion) (void)snprintf(piece, (size_t)required + 1, spec, (uintmax_t)unsigned_value);
        else (void)snprintf(piece, (size_t)required + 1, spec, real_value);
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

static CtValue scanned(CtInterpreter *interpreter, Token name, const CtValue *args, size_t count) {
    int from_string = named(name, "sscanf"), from_file = named(name, "fscanf");
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

static CtValue file_call(CtInterpreter *interpreter, Token name, const CtValue *args, size_t count) {
    if (!require_count(interpreter, name, count)) return integer(0);
    errno = 0;
    if (named(name, "fopen")) {
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
    if (named(name, "remove") || named(name, "rename") || named(name, "getenv") || named(name, "strerror")) {
        if (named(name, "strerror")) {
            int code = small(interpreter, name, args[0]);
            return interpreter->failed ? integer(0) : copy_string(interpreter, name, strerror(code));
        }
        char *path = string(interpreter, name, args[0]);
        char *target = named(name, "rename") ? string(interpreter, name, args[1]) : NULL;
        if (interpreter->failed) return integer(0);
        if (named(name, "getenv")) return copy_string(interpreter, name, getenv(path));
        int result = target ? rename(path, target) : remove(path);
        set_errno(interpreter, errno);
        return integer(result);
    }
    size_t stream_index = named(name, "fputc") || named(name, "fputs") || named(name, "putc") ||
                          named(name, "ungetc") ? 1 : named(name, "fgets") ? 2 :
                          named(name, "fread") || named(name, "fwrite") ? 3 : 0;
    if (named(name, "fflush") && ((args[0].type == CT_INT && args[0].as.integer == 0) ||
        (type_is_pointer(args[0].type) && args[0].as.address == 0))) return integer(fflush(NULL));
    HostFile *file = find_file(interpreter, name, args[stream_index]);
    if (!file) return integer(EOF);
    FILE *stream = file_stream(interpreter, file);
    int result = 0;
    if (named(name, "fclose")) {
        if (file->standard) return runtime_error(interpreter, name, "closing interpreter-owned standard streams is unsupported");
        result = fclose(stream);
        file->closed = 1;
        (void)memory_release(&interpreter->memory, file->address, 0);
    } else if (named(name, "fflush")) result = fflush(stream);
    else if (named(name, "fgetc") || named(name, "getc")) result = fgetc(stream);
    else if (named(name, "ungetc")) {
        int character = small(interpreter, name, args[0]);
        if (!interpreter->failed) result = ungetc(character, stream);
    } else if (named(name, "fputc") || named(name, "putc")) {
        int character = small(interpreter, name, args[0]);
        if (!interpreter->failed) result = fputc(character, stream);
    } else if (named(name, "fputs")) {
        char *text = string(interpreter, name, args[0]);
        if (text) result = fputs(text, stream);
    } else if (named(name, "fgets")) {
        uint64_t address = pointer(interpreter, name, args[0]);
        int size = small(interpreter, name, args[1]);
        if (size <= 0) return runtime_error(interpreter, name, "fgets size must be positive");
        char *target = access_memory(interpreter, name, address, (size_t)size, 2);
        if (!target) return integer(0);
        char *read = fgets(target, size, stream);
        if (read) (void)memory_access(&interpreter->memory, address, strlen(read) + 1, 1);
        set_errno(interpreter, errno);
        return (CtValue){.type = CHAR_POINTER, .as.address = read ? address : 0};
    } else if (named(name, "fread") || named(name, "fwrite")) {
        uint64_t address = pointer(interpreter, name, args[0]);
        size_t size = size_argument(interpreter, name, args[1]);
        size_t elements = size_argument(interpreter, name, args[2]);
        if (size && elements > SIZE_MAX / size) return runtime_error(interpreter, name, "file transfer size overflow");
        if (!size || !elements) return integer(0);
        int reading = named(name, "fread");
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
    } else if (named(name, "fseek")) {
        int offset = small(interpreter, name, args[1]);
        int origin = small(interpreter, name, args[2]);
        if (origin != SEEK_SET && origin != SEEK_CUR && origin != SEEK_END) return runtime_error(interpreter, name, "invalid seek origin");
        if (!interpreter->failed) result = fseek(stream, offset, origin);
    } else if (named(name, "ftell")) {
        long position = ftell(stream);
        set_errno(interpreter, errno);
        return integer_of(CT_LONG, position);
    } else if (named(name, "rewind")) rewind(stream);
    else if (named(name, "feof")) result = feof(stream);
    else if (named(name, "ferror")) result = ferror(stream);
    else if (named(name, "clearerr")) clearerr(stream);
    set_errno(interpreter, errno);
    if (named(name, "rewind") || named(name, "clearerr")) return (CtValue){.type = CT_VOID};
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

static CtValue va_formatted(CtInterpreter *interpreter, Token name, const CtValue *args, size_t count) {
    if (!require_count(interpreter, name, count)) return integer(0);
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
     * into interpreted memory. Dropping the leading v selects the base routine. */
    Token base = name;
    ++base.start;
    --base.length;
    int input = named(base, "scanf") || named(base, "sscanf") || named(base, "fscanf");
    CtValue result = input ? scanned(interpreter, base, values, fixed + extra)
                           : formatted(interpreter, base, values, fixed + extra);
    if (values != inline_values) free(values);
    return result;
}

CtValue builtin_call(CtInterpreter *interpreter, Token name, size_t id,
                     const CtValue *args, size_t count) {
    const Signature *signature = id < builtin_count() ? &signatures[id] : NULL;
    if (!signature || (signature->arity != VARIADIC && signature->arity != count))
        return runtime_error(interpreter, name, "incorrect number of library arguments");
    if (id >= BI_VPRINTF && id <= BI_VFSCANF) return va_formatted(interpreter, name, args, count);
    if (id <= BI_FPRINTF) return formatted(interpreter, name, args, count);
    if (id >= BI_SCANF && id <= BI_FSCANF) return scanned(interpreter, name, args, count);
    if (id >= BI_FOPEN && id <= BI_STRERROR) return file_call(interpreter, name, args, count);
    if ((id >= BI_STRLEN && id <= BI_STRPBRK) || (id >= BI_ATOI && id <= BI_ATOF)) goto string_functions;
    if (id >= BI_MEMCPY && id <= BI_MEMCHR) goto memory_functions;
    if (id == BI_STRTOL || id == BI_STRTOD) goto number_conversion;
    if (id == BI_STRTOK) goto tokenize;
    if (id == BI_QSORT || id == BI_BSEARCH) goto search_functions;
    if (id >= BI_ISDIGIT && id <= BI_TOLOWER) goto character_functions;
    if (id >= BI_SQRT && id <= BI_COPYSIGN) goto math_functions;
    if (named(name, "getchar")) return integer(fgetc(interpreter->input));
    if (named(name, "clock")) return integer_of(CT_LONG, (int64_t)clock());
    if (named(name, "rand")) {
        interpreter->random_state = interpreter->random_state * 1103515245u + 12345u;
        return integer((int)((interpreter->random_state / 65536u) % 32768u));
    }
    if (named(name, "abort")) return runtime_error(interpreter, name, "the program called abort");
    if (named(name, "interpret_depth")) return integer((int)interpreter->nesting);
    if (named(name, "interpret")) return interpret(interpreter, name, args[0]);
    if (named(name, "puts")) {
        char *text = string(interpreter, name, args[0]);
        if (!text) return integer(EOF);
        int result = fputs(text, interpreter->output);
        return integer(result < 0 || fputc('\n', interpreter->output) == EOF ? EOF : 0);
    }
    if (named(name, "putchar")) {
        int character = small(interpreter, name, args[0]);
        return integer(interpreter->failed ? EOF : fputc(character, interpreter->output));
    }
    if (named(name, "malloc") || named(name, "calloc") || named(name, "realloc")) {
        size_t size = size_argument(interpreter, name, args[named(name, "realloc") ? 1 : 0]);
        if (named(name, "calloc")) {
            size_t elements = size_argument(interpreter, name, args[1]);
            if (elements && size > SIZE_MAX / elements) return runtime_error(interpreter, name, "allocation size overflow");
            size *= elements;
        }
        uint64_t old_address = 0;
        Allocation *old = NULL;
        if (named(name, "realloc")) {
            old_address = pointer(interpreter, name, args[0]);
            if (old_address) {
                old = memory_find(&interpreter->memory, old_address);
                if (!old || !old->alive || !old->heap || old->address != old_address)
                    return runtime_error(interpreter, name, "realloc requires a live heap allocation");
            }
        }
        if (interpreter->failed) return integer(0);
        uint64_t address = memory_allocate(&interpreter->memory, size, named(name, "calloc"), 1);
        if (!address) return runtime_error(interpreter, name, interpreter->memory.error);
        if (old) {
            Allocation *target = memory_find(&interpreter->memory, address);
            size_t copied = old->size < size ? old->size : size;
            memcpy(target->data, old->data, copied);
            memcpy(target->initialized, old->initialized, copied);
            target->fully_initialized = old->fully_initialized && copied == size;
            (void)memory_release(&interpreter->memory, old_address, 1);
        }
        return (CtValue){.type = VOID_POINTER, .as.address = address};
    }
    if (named(name, "free")) {
        uint64_t address = pointer(interpreter, name, args[0]);
        if (!interpreter->failed && !memory_release(&interpreter->memory, address, 1))
            return runtime_error(interpreter, name, interpreter->memory.error);
        return (CtValue){.type = CT_VOID};
    }
string_functions:
    if (named(name, "strlen") || named(name, "strcmp") || named(name, "strncmp") || named(name, "strcpy") ||
        named(name, "strncpy") || named(name, "strcat") || named(name, "strncat") || named(name, "strchr") ||
        named(name, "strrchr") || named(name, "strstr") || named(name, "strspn") || named(name, "strcspn") ||
        named(name, "strpbrk") || named(name, "atoi") || named(name, "atol") || named(name, "atof")) {
        int copying = named(name, "strcpy") || named(name, "strncpy");
        int character_argument = named(name, "strchr") || named(name, "strrchr");
        char *a = copying ? NULL : string(interpreter, name, args[0]);
        char *b = count > 1 && !character_argument ? string(interpreter, name, args[1]) : NULL;
        if (interpreter->failed) return integer(0);
        if (named(name, "strlen")) return integer_of(CT_ULONG, (int64_t)strlen(a));
        if (named(name, "atoi") || named(name, "atol")) {
            errno = 0;
            long value = strtol(a, NULL, 10);
            int wide = named(name, "atol");
            if (errno == ERANGE || (!wide && (value < INT_MIN || value > INT_MAX)))
                return runtime_error(interpreter, name, "the parsed value is out of range");
            return integer_of(wide ? CT_LONG : CT_INT, value);
        }
        if (named(name, "atof")) {
            errno = 0;
            double value = strtod(a, NULL);
            if (errno == ERANGE || !isfinite(value)) return runtime_error(interpreter, name, "atof result out of range");
            return (CtValue){.type = CT_DOUBLE, .as.real = value};
        }
        if (named(name, "strcmp")) return integer(strcmp(a, b));
        if (named(name, "strncmp")) {
            size_t n = size_argument(interpreter, name, args[2]);
            return interpreter->failed ? integer(0) : integer(strncmp(a, b, n));
        }
        if (named(name, "strspn")) return integer_of(CT_ULONG, (int64_t)strspn(a, b));
        if (named(name, "strcspn")) return integer_of(CT_ULONG, (int64_t)strcspn(a, b));
        if (named(name, "strchr") || named(name, "strrchr") || named(name, "strstr") || named(name, "strpbrk")) {
            int character = named(name, "strstr") || named(name, "strpbrk") ? 0 : small(interpreter, name, args[1]);
            if (interpreter->failed) return integer(0);
            char *found = named(name, "strchr") ? strchr(a, character)
                        : named(name, "strrchr") ? strrchr(a, character)
                        : named(name, "strpbrk") ? strpbrk(a, b) : strstr(a, b);
            return (CtValue){.type = CHAR_POINTER, .as.address = found ? args[0].as.address + (uint64_t)(found - a) : 0};
        }
        uint64_t address = pointer(interpreter, name, args[0]);
        int appending = named(name, "strcat") || named(name, "strncat");
        size_t prefix = appending ? strlen(a) : 0;
        size_t limit = named(name, "strncpy") || named(name, "strncat") ? size_argument(interpreter, name, args[2]) : 0;
        size_t bytes = named(name, "strncpy") ? limit
                     : named(name, "strncat") ? (strlen(b) < limit ? strlen(b) : limit) + 1
                     : strlen(b) + 1;
        char *destination = access_memory(interpreter, name, address + prefix, bytes, 1);
        if (destination) {
            if (named(name, "strncpy")) {
                size_t copied = strlen(b) < bytes ? strlen(b) : bytes;
                memmove(destination, b, copied);
                memset(destination + copied, 0, bytes - copied);
            } else if (named(name, "strncat")) {
                memmove(destination, b, bytes - 1);
                destination[bytes - 1] = '\0';
            } else memmove(destination, b, bytes);
        }
        return (CtValue){.type = CHAR_POINTER, .as.address = address};
    }
memory_functions:
    if (named(name, "memchr")) {
        uint64_t address = pointer(interpreter, name, args[0]);
        int character = small(interpreter, name, args[1]);
        size_t bytes = size_argument(interpreter, name, args[2]);
        void *data = access_memory(interpreter, name, address, bytes, 0);
        if (!data) return integer(0);
        unsigned char *found = memchr(data, character, bytes);
        return (CtValue){.type = VOID_POINTER, .as.address = found ? address + (uint64_t)(found - (unsigned char *)data) : 0};
    }
    if (named(name, "memcpy") || named(name, "memmove") || named(name, "memset") || named(name, "memcmp")) {
        uint64_t destination = pointer(interpreter, name, args[0]);
        size_t bytes = size_argument(interpreter, name, args[2]);
        int setting = named(name, "memset"), comparing = named(name, "memcmp");
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
    if (named(name, "perror")) {
        char *prefix = string(interpreter, name, args[0]);
        if (!interpreter->failed)
            fprintf(interpreter->errors, "%s%s%s\n", prefix && *prefix ? prefix : "",
                    prefix && *prefix ? ": " : "", strerror(get_errno(interpreter)));
        return (CtValue){.type = CT_VOID};
    }
    if (named(name, "time")) {
        time_t now = time(NULL);
        if (now == (time_t)-1) return runtime_error(interpreter, name, "the clock is unavailable");
        CtValue seconds = integer_of(CT_LONG, (int64_t)now);
        uint64_t destination = pointer(interpreter, name, args[0]);
        if (destination && !memory_write(&interpreter->memory, destination, seconds))
            return runtime_error(interpreter, name, interpreter->memory.error);
        return seconds;
    }
    if (named(name, "__assert_fail")) {
        char *expression = string(interpreter, name, args[0]);
        char *file = string(interpreter, name, args[1]);
        int line = small(interpreter, name, args[2]);
        if (interpreter->failed) return integer(0);
        char message[192];
        (void)snprintf(message, sizeof message, "assertion failed: %s (%s:%d)", expression, file, line);
        return runtime_error(interpreter, name, message);
    }
number_conversion:
    if (named(name, "strtol") || named(name, "strtod")) {
        char *text = string(interpreter, name, args[0]);
        int base = named(name, "strtol") ? small(interpreter, name, args[2]) : 0;
        if (interpreter->failed) return integer(0);
        if (named(name, "strtol") && base != 0 && (base < 2 || base > 36))
            return runtime_error(interpreter, name, "strtol requires a base of 0 or 2 through 36");
        char *end = NULL;
        errno = 0;
        double real_result = 0;
        long integer_result = 0;
        if (named(name, "strtol")) integer_result = strtol(text, &end, base);
        else real_result = strtod(text, &end);
        set_errno(interpreter, errno);
        uint64_t destination = pointer(interpreter, name, args[1]);
        if (destination) {
            CtValue position = {.type = CHAR_POINTER, .as.address = args[0].as.address + (uint64_t)(end - text)};
            if (!memory_write(&interpreter->memory, destination, position))
                return runtime_error(interpreter, name, interpreter->memory.error);
        }
        if (named(name, "strtod")) return (CtValue){.type = CT_DOUBLE, .as.real = real_result};
        return integer_of(CT_LONG, integer_result);
    }
tokenize:
    if (named(name, "strtok")) {
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
    if (named(name, "qsort") || named(name, "bsearch")) {
        int searching = named(name, "bsearch");
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
        unsigned char *scratch = malloc(width);
        if (!scratch) return runtime_error(interpreter, name, "out of memory");
        for (size_t gap = elements / 2; gap && !interpreter->failed; gap /= 2)
            for (size_t i = gap; i < elements && !interpreter->failed; ++i)
                for (size_t j = i; j >= gap; j -= gap) {
                    pair[0] = (CtValue){.type = VOID_POINTER, .as.address = base + (j - gap) * width};
                    pair[1] = (CtValue){.type = VOID_POINTER, .as.address = base + j * width};
                    CtValue order = runtime_invoke(interpreter, name, compare, pair, 2);
                    if (interpreter->failed || order.as.integer <= 0) break;
                    memcpy(scratch, data + (j - gap) * width, width);
                    memcpy(data + (j - gap) * width, data + j * width, width);
                    memcpy(data + j * width, scratch, width);
                }
        free(scratch);
        return (CtValue){.type = CT_VOID};
    }
    if (named(name, "exit") || named(name, "srand") || named(name, "abs") || named(name, "labs")) {
        if (named(name, "labs")) {
            int64_t wide = number(interpreter, name, args[0]);
            if (interpreter->failed) return integer(0);
            if (wide == INT64_MIN) return runtime_error(interpreter, name, "labs result overflows long");
            return integer_of(CT_LONG, wide < 0 ? -wide : wide);
        }
        int value = small(interpreter, name, args[0]);
        if (interpreter->failed) return integer(0);
        if (named(name, "exit")) { interpreter->exit_requested = 1; interpreter->exit_status = value; return (CtValue){.type = CT_VOID}; }
        if (named(name, "srand")) { interpreter->random_state = (unsigned)value; return (CtValue){.type = CT_VOID}; }
        if (value == INT_MIN) return runtime_error(interpreter, name, "abs result overflows int");
        return integer_of(named(name, "labs") ? CT_LONG : CT_INT, abs(value));
    }
character_functions:
    if ((name.length >= 2 && !memcmp(name.start, "is", 2)) || named(name, "toupper") || named(name, "tolower")) {
        int value = small(interpreter, name, args[0]);
        if (value != EOF && (value < 0 || value > UCHAR_MAX)) return runtime_error(interpreter, name, "ctype requires an unsigned char or EOF");
        if (interpreter->failed) return integer(0);
        if (named(name, "isdigit")) return integer(isdigit(value));
        if (named(name, "isalpha")) return integer(isalpha(value));
        if (named(name, "isalnum")) return integer(isalnum(value));
        if (named(name, "isspace")) return integer(isspace(value));
        if (named(name, "isupper")) return integer(isupper(value));
        if (named(name, "islower")) return integer(islower(value));
        if (named(name, "ispunct")) return integer(ispunct(value));
        if (named(name, "isprint")) return integer(isprint(value));
        if (named(name, "isgraph")) return integer(isgraph(value));
        if (named(name, "iscntrl")) return integer(iscntrl(value));
        if (named(name, "isxdigit")) return integer(isxdigit(value));
        if (named(name, "isblank")) return integer(value == ' ' || value == '\t');
        return integer(named(name, "toupper") ? toupper(value) : tolower(value));
    }
math_functions:
    (void)0;
    double a = real_number(interpreter, name, args[0]);
    double b = count == 2 ? real_number(interpreter, name, args[1]) : 0.0;
    if (interpreter->failed) return integer(0);
    double value = 0;
    errno = 0;
    if (named(name, "sqrt")) value = sqrt(a);
    else if (named(name, "cbrt")) value = cbrt(a);
    else if (named(name, "hypot")) value = hypot(a, b);
    else if (named(name, "sinh")) value = sinh(a);
    else if (named(name, "cosh")) value = cosh(a);
    else if (named(name, "tanh")) value = tanh(a);
    else if (named(name, "asinh")) value = asinh(a);
    else if (named(name, "acosh")) value = acosh(a);
    else if (named(name, "atanh")) value = atanh(a);
    else if (named(name, "exp2")) value = exp2(a);
    else if (named(name, "log2")) value = log2(a);
    else if (named(name, "ldexp")) value = ldexp(a, (int)b);
    else if (named(name, "fmin")) value = fmin(a, b);
    else if (named(name, "fmax")) value = fmax(a, b);
    else if (named(name, "fdim")) value = fdim(a, b);
    else if (named(name, "copysign")) value = copysign(a, b);
    else if (named(name, "difftime")) value = a - b;
    else if (named(name, "pow")) value = pow(a, b);
    else if (named(name, "sin")) value = sin(a);
    else if (named(name, "cos")) value = cos(a);
    else if (named(name, "tan")) value = tan(a);
    else if (named(name, "asin")) value = asin(a);
    else if (named(name, "acos")) value = acos(a);
    else if (named(name, "atan")) value = atan(a);
    else if (named(name, "atan2")) value = atan2(a, b);
    else if (named(name, "exp")) value = exp(a);
    else if (named(name, "log")) value = log(a);
    else if (named(name, "log10")) value = log10(a);
    else if (named(name, "floor")) value = floor(a);
    else if (named(name, "ceil")) value = ceil(a);
    else if (named(name, "round")) value = round(a);
    else if (named(name, "trunc")) value = trunc(a);
    else if (named(name, "fabs")) value = fabs(a);
    else if (named(name, "fmod")) value = fmod(a, b);
    else return runtime_error(interpreter, name, "unknown library function");
    if (errno == EDOM || errno == ERANGE || !isfinite(value)) return runtime_error(interpreter, name, "math result is outside its domain or range");
    return (CtValue){.type = CT_DOUBLE, .as.real = value};
}
