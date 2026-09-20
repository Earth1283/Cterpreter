#include "runtime.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHAR_POINTER type_pointer(CT_CHAR)
#define VOID_POINTER type_pointer(CT_VOID)

typedef enum { RT_INT, RT_DOUBLE, RT_VOID, RT_CHAR_POINTER, RT_VOID_POINTER } ReturnKind;

static CtValue integer(int value) { return (CtValue){.type = CT_INT, .as.integer = value}; }
static int named(Token token, const char *name) { return strlen(name) == token.length && !memcmp(token.start, name, token.length); }

static CtType return_type(ReturnKind kind) {
    switch (kind) {
        case RT_DOUBLE: return CT_DOUBLE;
        case RT_VOID: return CT_VOID;
        case RT_CHAR_POINTER: return type_pointer(CT_CHAR);
        case RT_VOID_POINTER: return type_pointer(CT_VOID);
        default: return CT_INT;
    }
}

#define VARIADIC 255

static const struct { const char *name; ReturnKind type; unsigned char arity; } signatures[] = {
    {"printf", RT_INT, VARIADIC}, {"sprintf", RT_INT, VARIADIC}, {"snprintf", RT_INT, VARIADIC},
    {"fprintf", RT_INT, VARIADIC}, {"scanf", RT_INT, VARIADIC}, {"sscanf", RT_INT, VARIADIC},
    {"fscanf", RT_INT, VARIADIC},
    {"puts", RT_INT, 1}, {"putchar", RT_INT, 1}, {"getchar", RT_INT, 0}, {"perror", RT_VOID, 1},
    {"fopen", RT_VOID_POINTER, 2}, {"fclose", RT_INT, 1}, {"fflush", RT_INT, 1}, {"fgetc", RT_INT, 1},
    {"fputc", RT_INT, 2}, {"fputs", RT_INT, 2}, {"fgets", RT_CHAR_POINTER, 3},
    {"getc", RT_INT, 1}, {"putc", RT_INT, 2}, {"ungetc", RT_INT, 2},
    {"fread", RT_INT, 4}, {"fwrite", RT_INT, 4}, {"fseek", RT_INT, 3}, {"ftell", RT_INT, 1},
    {"rewind", RT_VOID, 1}, {"feof", RT_INT, 1}, {"ferror", RT_INT, 1}, {"clearerr", RT_VOID, 1},
    {"remove", RT_INT, 1}, {"rename", RT_INT, 2}, {"getenv", RT_CHAR_POINTER, 1}, {"strerror", RT_CHAR_POINTER, 1},
    {"malloc", RT_VOID_POINTER, 1}, {"calloc", RT_VOID_POINTER, 2}, {"realloc", RT_VOID_POINTER, 2},
    {"free", RT_VOID, 1},
    {"strlen", RT_INT, 1}, {"strcmp", RT_INT, 2}, {"strncmp", RT_INT, 3}, {"strcpy", RT_CHAR_POINTER, 2},
    {"strncpy", RT_CHAR_POINTER, 3}, {"strcat", RT_CHAR_POINTER, 2}, {"strncat", RT_CHAR_POINTER, 3},
    {"strchr", RT_CHAR_POINTER, 2}, {"strrchr", RT_CHAR_POINTER, 2}, {"strstr", RT_CHAR_POINTER, 2},
    {"strspn", RT_INT, 2}, {"strcspn", RT_INT, 2}, {"strpbrk", RT_CHAR_POINTER, 2}, {"strtok", RT_CHAR_POINTER, 2},
    {"memcpy", RT_VOID_POINTER, 3}, {"memmove", RT_VOID_POINTER, 3}, {"memset", RT_VOID_POINTER, 3},
    {"memcmp", RT_INT, 3}, {"memchr", RT_VOID_POINTER, 3},
    {"atoi", RT_INT, 1}, {"atol", RT_INT, 1}, {"atof", RT_DOUBLE, 1},
    {"strtol", RT_INT, 3}, {"strtod", RT_DOUBLE, 2},
    {"abs", RT_INT, 1}, {"labs", RT_INT, 1}, {"rand", RT_INT, 0}, {"srand", RT_VOID, 1},
    {"qsort", RT_VOID, 4}, {"bsearch", RT_VOID_POINTER, 5}, {"abort", RT_VOID, 0}, {"exit", RT_VOID, 1},
    {"sqrt", RT_DOUBLE, 1}, {"cbrt", RT_DOUBLE, 1}, {"pow", RT_DOUBLE, 2}, {"hypot", RT_DOUBLE, 2},
    {"sin", RT_DOUBLE, 1}, {"cos", RT_DOUBLE, 1}, {"tan", RT_DOUBLE, 1},
    {"asin", RT_DOUBLE, 1}, {"acos", RT_DOUBLE, 1}, {"atan", RT_DOUBLE, 1}, {"atan2", RT_DOUBLE, 2},
    {"sinh", RT_DOUBLE, 1}, {"cosh", RT_DOUBLE, 1}, {"tanh", RT_DOUBLE, 1},
    {"asinh", RT_DOUBLE, 1}, {"acosh", RT_DOUBLE, 1}, {"atanh", RT_DOUBLE, 1},
    {"exp", RT_DOUBLE, 1}, {"exp2", RT_DOUBLE, 1}, {"log", RT_DOUBLE, 1}, {"log2", RT_DOUBLE, 1},
    {"log10", RT_DOUBLE, 1}, {"ldexp", RT_DOUBLE, 2},
    {"floor", RT_DOUBLE, 1}, {"ceil", RT_DOUBLE, 1}, {"round", RT_DOUBLE, 1}, {"trunc", RT_DOUBLE, 1},
    {"fabs", RT_DOUBLE, 1}, {"fmod", RT_DOUBLE, 2}, {"fmin", RT_DOUBLE, 2}, {"fmax", RT_DOUBLE, 2},
    {"fdim", RT_DOUBLE, 2}, {"copysign", RT_DOUBLE, 2},
    {"time", RT_INT, 1}, {"difftime", RT_DOUBLE, 2}, {"clock", RT_INT, 0},
    {"isdigit", RT_INT, 1}, {"isalpha", RT_INT, 1}, {"isalnum", RT_INT, 1}, {"isspace", RT_INT, 1},
    {"isupper", RT_INT, 1}, {"islower", RT_INT, 1}, {"ispunct", RT_INT, 1}, {"isprint", RT_INT, 1},
    {"isgraph", RT_INT, 1}, {"iscntrl", RT_INT, 1}, {"isxdigit", RT_INT, 1}, {"isblank", RT_INT, 1},
    {"toupper", RT_INT, 1}, {"tolower", RT_INT, 1},
    {"__assert_fail", RT_INT, 3},
    {"interpret", RT_INT, 1}, {"interpret_depth", RT_INT, 0}
};

int builtin_type(Token name, CtType *type) {
    for (size_t i = 0; i < sizeof signatures / sizeof signatures[0]; ++i)
        if (named(name, signatures[i].name)) { *type = return_type(signatures[i].type); return 1; }
    return 0;
}

static int require_count(CtInterpreter *interpreter, Token name, size_t count) {
    for (size_t i = 0; i < sizeof signatures / sizeof signatures[0]; ++i)
        if (named(name, signatures[i].name)) {
            if (signatures[i].arity == VARIADIC || signatures[i].arity == count) return 1;
            break;
        }
    (void)runtime_error(interpreter, name, "incorrect number of library arguments");
    return 0;
}

static int number(CtInterpreter *interpreter, Token name, CtValue value) {
    if (value.type != CT_INT && value.type != CT_CHAR) {
        (void)runtime_error(interpreter, name, "library argument requires an integer");
        return 0;
    }
    return value.as.integer;
}

static double real_number(CtInterpreter *interpreter, Token name, CtValue value) {
    if (value.type == CT_DOUBLE) return value.as.real;
    return (double)number(interpreter, name, value);
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
    int size = number(interpreter, name, value);
    if (size < 0) (void)runtime_error(interpreter, name, "size cannot be negative");
    return size < 0 ? 0 : (size_t)size;
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
    return interpreter->memory.error ? 0 : value.as.integer;
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
            width = number(interpreter, name, args[argument++]);
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
                precision = number(interpreter, name, args[argument++]);
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
        int signed_value = 0;
        unsigned int unsigned_value = 0;
        char pointer_text[32];
        double real_value = 0;
        uint64_t address = 0;
        int integer_conversion = strchr("diuoxX", conversion) != NULL;
        if (integer_conversion || conversion == 'c') signed_value = number(interpreter, name, value);
        else if (conversion == 's') text = string(interpreter, name, value);
        else if (conversion == 'p') address = pointer(interpreter, name, value);
        else real_value = real_number(interpreter, name, value);
        unsigned_value = (unsigned int)signed_value;
        if (length == 'h' && integer_conversion) {
            unsigned_value = doubled ? (unsigned char)signed_value : (unsigned short)signed_value;
            signed_value = doubled ? (signed char)signed_value : (short)signed_value;
        }
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
        else if (conversion == 'c') required = snprintf(NULL, 0, spec, signed_value);
        else if (conversion == 'd' || conversion == 'i') required = snprintf(NULL, 0, spec, (intmax_t)signed_value);
        else if (integer_conversion) required = snprintf(NULL, 0, spec, (uintmax_t)unsigned_value);
        else required = snprintf(NULL, 0, spec, real_value);
        if (required < 0 || required > (int)CT_SOURCE_LIMIT) { (void)runtime_error(interpreter, name, "formatted output limit exceeded"); break; }
        char *piece = malloc((size_t)required + 1);
        if (!piece) { (void)runtime_error(interpreter, name, "out of memory"); break; }
        if (conversion == 's') (void)snprintf(piece, (size_t)required + 1, spec, text);
        else if (conversion == 'c') (void)snprintf(piece, (size_t)required + 1, spec, signed_value);
        else if (conversion == 'd' || conversion == 'i') (void)snprintf(piece, (size_t)required + 1, spec, (intmax_t)signed_value);
        else if (integer_conversion) (void)snprintf(piece, (size_t)required + 1, spec, (uintmax_t)unsigned_value);
        else (void)snprintf(piece, (size_t)required + 1, spec, real_value);
        if (!append(&output, piece, (size_t)required)) (void)runtime_error(interpreter, name, "formatted output limit exceeded");
        free(piece);
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
        int long_format = format[i] == 'l';
        if (long_format) ++i;
        char conversion = format[i];
        if (!conversion || !strchr("diuoxXfFeEgGscn", conversion)) return runtime_error(interpreter, name, "unsupported scanf conversion");
        int floating = strchr("fFeEgG", conversion) != NULL;
        if ((floating && !long_format) || (!floating && long_format))
            return runtime_error(interpreter, name, "scanf supports int, char, and %lf double destinations");
        CtType type = floating ? CT_DOUBLE : conversion == 's' || conversion == 'c' ? CT_CHAR : CT_INT;
        uint64_t destination = 0;
        if (!suppress) {
            if (argument >= count) return runtime_error(interpreter, name, "not enough scanf destinations");
            if (args[argument].type != type_pointer(type))
                return runtime_error(interpreter, name, "scanf destination type does not match its format");
            destination = args[argument++].as.address;
        }
        if (conversion == 'n') {
            if (!suppress && !memory_write(&interpreter->memory, destination, integer((int)input.position)))
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
            if (floating) value = (CtValue){.type = CT_DOUBLE, .as.real = strtod(text, &end)};
            else {
                int base = conversion == 'i' ? 0 : conversion == 'o' ? 8 : conversion == 'x' || conversion == 'X' ? 16 : 10;
                long number_value = strtol(text, &end, base);
                if (number_value < INT_MIN || number_value > INT_MAX) errno = ERANGE;
                value = integer((int)number_value);
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
            int code = number(interpreter, name, args[0]);
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
        int character = number(interpreter, name, args[0]);
        if (!interpreter->failed) result = ungetc(character, stream);
    } else if (named(name, "fputc") || named(name, "putc")) {
        int character = number(interpreter, name, args[0]);
        if (!interpreter->failed) result = fputc(character, stream);
    } else if (named(name, "fputs")) {
        char *text = string(interpreter, name, args[0]);
        if (text) result = fputs(text, stream);
    } else if (named(name, "fgets")) {
        uint64_t address = pointer(interpreter, name, args[0]);
        int size = number(interpreter, name, args[1]);
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
        result = (int)transferred;
    } else if (named(name, "fseek")) {
        int offset = number(interpreter, name, args[1]);
        int origin = number(interpreter, name, args[2]);
        if (origin != SEEK_SET && origin != SEEK_CUR && origin != SEEK_END) return runtime_error(interpreter, name, "invalid seek origin");
        if (!interpreter->failed) result = fseek(stream, offset, origin);
    } else if (named(name, "ftell")) {
        long position = ftell(stream);
        if (position > INT_MAX) return runtime_error(interpreter, name, "file position exceeds supported int range");
        result = (int)position;
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
    ct_set_streams(child, interpreter->input, interpreter->output, interpreter->errors);
    ct_set_interrupt(child, interpreter->interrupt);
    ct_set_limits(child, budget, interpreter->depth_limit);
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

CtValue builtin_call(CtInterpreter *interpreter, Token name, const CtValue *args, size_t count) {
    if (named(name, "printf") || named(name, "sprintf") || named(name, "snprintf") || named(name, "fprintf")) return formatted(interpreter, name, args, count);
    if (named(name, "scanf") || named(name, "sscanf") || named(name, "fscanf")) return scanned(interpreter, name, args, count);
    if (named(name, "fopen") || named(name, "fclose") || named(name, "fflush") || named(name, "fgetc") ||
        named(name, "fputc") || named(name, "fputs") || named(name, "fgets") || named(name, "fread") ||
        named(name, "fwrite") || named(name, "fseek") || named(name, "ftell") || named(name, "rewind") ||
        named(name, "feof") || named(name, "ferror") || named(name, "clearerr") || named(name, "remove") ||
        named(name, "rename") || named(name, "getenv") || named(name, "strerror") ||
        named(name, "getc") || named(name, "putc") || named(name, "ungetc")) return file_call(interpreter, name, args, count);
    if (!require_count(interpreter, name, count)) return integer(0);
    if (named(name, "getchar")) return integer(fgetc(interpreter->input));
    if (named(name, "clock")) return integer((int)clock());
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
        int character = number(interpreter, name, args[0]);
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
    if (named(name, "strlen") || named(name, "strcmp") || named(name, "strncmp") || named(name, "strcpy") ||
        named(name, "strncpy") || named(name, "strcat") || named(name, "strncat") || named(name, "strchr") ||
        named(name, "strrchr") || named(name, "strstr") || named(name, "strspn") || named(name, "strcspn") ||
        named(name, "strpbrk") || named(name, "atoi") || named(name, "atol") || named(name, "atof")) {
        int copying = named(name, "strcpy") || named(name, "strncpy");
        int character_argument = named(name, "strchr") || named(name, "strrchr");
        char *a = copying ? NULL : string(interpreter, name, args[0]);
        char *b = count > 1 && !character_argument ? string(interpreter, name, args[1]) : NULL;
        if (interpreter->failed) return integer(0);
        if (named(name, "strlen")) return integer((int)strlen(a));
        if (named(name, "atoi") || named(name, "atol")) {
            errno = 0;
            long value = strtol(a, NULL, 10);
            if (errno == ERANGE || value < INT_MIN || value > INT_MAX) return runtime_error(interpreter, name, "atoi result is out of range");
            return integer((int)value);
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
        if (named(name, "strspn")) return integer((int)strspn(a, b));
        if (named(name, "strcspn")) return integer((int)strcspn(a, b));
        if (named(name, "strchr") || named(name, "strrchr") || named(name, "strstr") || named(name, "strpbrk")) {
            int character = named(name, "strstr") || named(name, "strpbrk") ? 0 : number(interpreter, name, args[1]);
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
    if (named(name, "memchr")) {
        uint64_t address = pointer(interpreter, name, args[0]);
        int character = number(interpreter, name, args[1]);
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
        int character = setting ? number(interpreter, name, args[1]) : 0;
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
        if (now == (time_t)-1 || (double)now > (double)INT_MAX)
            return runtime_error(interpreter, name, "the clock is outside the supported int range");
        uint64_t destination = pointer(interpreter, name, args[0]);
        if (destination && !memory_write(&interpreter->memory, destination, integer((int)now)))
            return runtime_error(interpreter, name, interpreter->memory.error);
        return integer((int)now);
    }
    if (named(name, "__assert_fail")) {
        char *expression = string(interpreter, name, args[0]);
        char *file = string(interpreter, name, args[1]);
        int line = number(interpreter, name, args[2]);
        if (interpreter->failed) return integer(0);
        char message[192];
        (void)snprintf(message, sizeof message, "assertion failed: %s (%s:%d)", expression, file, line);
        return runtime_error(interpreter, name, message);
    }
    if (named(name, "strtol") || named(name, "strtod")) {
        char *text = string(interpreter, name, args[0]);
        int base = named(name, "strtol") ? number(interpreter, name, args[2]) : 0;
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
        if (integer_result < INT_MIN || integer_result > INT_MAX)
            return runtime_error(interpreter, name, "strtol result is outside the supported int range");
        return integer((int)integer_result);
    }
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
        int value = number(interpreter, name, args[0]);
        if (interpreter->failed) return integer(0);
        if (named(name, "exit")) { interpreter->exit_requested = 1; interpreter->exit_status = value; return (CtValue){.type = CT_VOID}; }
        if (named(name, "srand")) { interpreter->random_state = (unsigned)value; return (CtValue){.type = CT_VOID}; }
        if (value == INT_MIN) return runtime_error(interpreter, name, "abs result overflows int");
        return integer(abs(value));
    }
    if ((name.length >= 2 && !memcmp(name.start, "is", 2)) || named(name, "toupper") || named(name, "tolower")) {
        int value = number(interpreter, name, args[0]);
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
