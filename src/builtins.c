#include "runtime.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHAR_POINTER ((CtType)(CT_CHAR + CT_POINTER))
#define VOID_POINTER ((CtType)(CT_VOID + CT_POINTER))

static CtValue integer(int value) { return (CtValue){.type = CT_INT, .as.integer = value}; }
static int named(Token token, const char *name) { return strlen(name) == token.length && !memcmp(token.start, name, token.length); }

static const struct { const char *name; CtType type; } signatures[] = {
    {"printf", CT_INT}, {"puts", CT_INT}, {"putchar", CT_INT}, {"getchar", CT_INT},
    {"sprintf", CT_INT}, {"snprintf", CT_INT}, {"fprintf", CT_INT},
    {"scanf", CT_INT}, {"sscanf", CT_INT}, {"fscanf", CT_INT},
    {"fopen", VOID_POINTER}, {"fclose", CT_INT}, {"fflush", CT_INT}, {"fgetc", CT_INT},
    {"fputc", CT_INT}, {"fputs", CT_INT}, {"fgets", CHAR_POINTER},
    {"fread", CT_INT}, {"fwrite", CT_INT}, {"fseek", CT_INT}, {"ftell", CT_INT},
    {"rewind", CT_VOID}, {"feof", CT_INT}, {"ferror", CT_INT}, {"clearerr", CT_VOID},
    {"remove", CT_INT}, {"rename", CT_INT}, {"getenv", CHAR_POINTER}, {"strerror", CHAR_POINTER},
    {"malloc", VOID_POINTER}, {"calloc", VOID_POINTER}, {"realloc", VOID_POINTER}, {"free", CT_VOID},
    {"strlen", CT_INT}, {"strcmp", CT_INT}, {"strncmp", CT_INT}, {"strcpy", CHAR_POINTER},
    {"strncpy", CHAR_POINTER}, {"strcat", CHAR_POINTER}, {"strchr", CHAR_POINTER}, {"strstr", CHAR_POINTER},
    {"memcpy", VOID_POINTER}, {"memmove", VOID_POINTER}, {"memset", VOID_POINTER}, {"memcmp", CT_INT},
    {"atoi", CT_INT}, {"atof", CT_DOUBLE}, {"abs", CT_INT}, {"rand", CT_INT}, {"srand", CT_VOID},
    {"exit", CT_VOID}, {"sqrt", CT_DOUBLE}, {"pow", CT_DOUBLE}, {"sin", CT_DOUBLE}, {"cos", CT_DOUBLE},
    {"tan", CT_DOUBLE}, {"asin", CT_DOUBLE}, {"acos", CT_DOUBLE}, {"atan", CT_DOUBLE},
    {"exp", CT_DOUBLE}, {"log", CT_DOUBLE}, {"log10", CT_DOUBLE}, {"floor", CT_DOUBLE},
    {"ceil", CT_DOUBLE}, {"round", CT_DOUBLE}, {"trunc", CT_DOUBLE}, {"fabs", CT_DOUBLE},
    {"fmod", CT_DOUBLE}, {"atan2", CT_DOUBLE}, {"clock", CT_INT},
    {"isdigit", CT_INT}, {"isalpha", CT_INT}, {"isalnum", CT_INT}, {"isspace", CT_INT},
    {"isupper", CT_INT}, {"islower", CT_INT}, {"toupper", CT_INT}, {"tolower", CT_INT}
};

int builtin_type(Token name, CtType *type) {
    for (size_t i = 0; i < sizeof signatures / sizeof signatures[0]; ++i)
        if (named(name, signatures[i].name)) { *type = signatures[i].type; return 1; }
    return 0;
}

static int require_count(CtInterpreter *interpreter, Token name, size_t count, size_t expected) {
    if (count == expected) return 1;
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
    if (value.type >= CT_POINTER) return value.as.address;
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

int builtin_value(CtInterpreter *interpreter, Token name, CtValue *value) {
    if (named(name, "errno")) { *value = integer(interpreter->error_number); return 1; }
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
            if (args[argument].type != (CtType)(type + CT_POINTER))
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
    size_t expected = 1;
    if (named(name, "fopen") || named(name, "fputc") || named(name, "fputs") || named(name, "rename")) expected = 2;
    if (named(name, "fgets") || named(name, "fseek")) expected = 3;
    if (named(name, "fread") || named(name, "fwrite")) expected = 4;
    if (!require_count(interpreter, name, count, expected)) return integer(0);
    errno = 0;
    if (named(name, "fopen")) {
        char *path = string(interpreter, name, args[0]);
        char *mode = string(interpreter, name, args[1]);
        if (interpreter->failed) return integer(0);
        FILE *stream = fopen(path, mode);
        interpreter->error_number = errno;
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
        interpreter->error_number = errno;
        return integer(result);
    }
    size_t stream_index = named(name, "fputc") || named(name, "fputs") ? 1 : named(name, "fgets") ? 2 :
                          named(name, "fread") || named(name, "fwrite") ? 3 : 0;
    if (named(name, "fflush") && ((args[0].type == CT_INT && args[0].as.integer == 0) ||
        (args[0].type >= CT_POINTER && args[0].as.address == 0))) return integer(fflush(NULL));
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
    else if (named(name, "fgetc")) result = fgetc(stream);
    else if (named(name, "fputc")) {
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
        interpreter->error_number = errno;
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
    interpreter->error_number = errno;
    if (named(name, "rewind") || named(name, "clearerr")) return (CtValue){.type = CT_VOID};
    return integer(result);
}

CtValue builtin_call(CtInterpreter *interpreter, Token name, const CtValue *args, size_t count) {
    if (named(name, "printf") || named(name, "sprintf") || named(name, "snprintf") || named(name, "fprintf")) return formatted(interpreter, name, args, count);
    if (named(name, "scanf") || named(name, "sscanf") || named(name, "fscanf")) return scanned(interpreter, name, args, count);
    if (named(name, "fopen") || named(name, "fclose") || named(name, "fflush") || named(name, "fgetc") ||
        named(name, "fputc") || named(name, "fputs") || named(name, "fgets") || named(name, "fread") ||
        named(name, "fwrite") || named(name, "fseek") || named(name, "ftell") || named(name, "rewind") ||
        named(name, "feof") || named(name, "ferror") || named(name, "clearerr") || named(name, "remove") ||
        named(name, "rename") || named(name, "getenv") || named(name, "strerror")) return file_call(interpreter, name, args, count);
    if (named(name, "getchar") || named(name, "rand") || named(name, "clock")) {
        if (!require_count(interpreter, name, count, 0)) return integer(0);
        if (named(name, "getchar")) return integer(fgetc(interpreter->input));
        if (named(name, "rand")) {
            interpreter->random_state = interpreter->random_state * 1103515245u + 12345u;
            return integer((int)((interpreter->random_state / 65536u) % 32768u));
        }
        return integer((int)clock());
    }
    size_t expected = 1;
    if (named(name, "calloc") || named(name, "realloc") || named(name, "strcmp") || named(name, "strcpy") ||
        named(name, "strcat") || named(name, "strchr") || named(name, "strstr") || named(name, "pow") ||
        named(name, "fmod") || named(name, "atan2")) expected = 2;
    if (named(name, "strncmp") || named(name, "strncpy") || named(name, "memcpy") || named(name, "memmove") ||
        named(name, "memset") || named(name, "memcmp")) expected = 3;
    if (!require_count(interpreter, name, count, expected)) return integer(0);
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
        named(name, "strncpy") || named(name, "strcat") || named(name, "strchr") || named(name, "strstr") ||
        named(name, "atoi") || named(name, "atof")) {
        int copying = named(name, "strcpy") || named(name, "strncpy");
        char *a = copying ? NULL : string(interpreter, name, args[0]);
        char *b = count > 1 && !named(name, "strchr") ? string(interpreter, name, args[1]) : NULL;
        if (interpreter->failed) return integer(0);
        if (named(name, "strlen")) return integer((int)strlen(a));
        if (named(name, "atoi")) {
            errno = 0;
            long value = strtol(a, NULL, 10);
            if (errno == ERANGE || value < INT_MIN || value > INT_MAX) return runtime_error(interpreter, name, "atoi result out of range");
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
        if (named(name, "strchr") || named(name, "strstr")) {
            int character = named(name, "strchr") ? number(interpreter, name, args[1]) : 0;
            if (interpreter->failed) return integer(0);
            char *found = named(name, "strchr") ? strchr(a, character) : strstr(a, b);
            return (CtValue){.type = CHAR_POINTER, .as.address = found ? args[0].as.address + (uint64_t)(found - a) : 0};
        }
        uint64_t address = pointer(interpreter, name, args[0]);
        size_t prefix = named(name, "strcat") ? strlen(a) : 0;
        size_t bytes = named(name, "strncpy") ? size_argument(interpreter, name, args[2]) : strlen(b) + 1;
        char *destination = access_memory(interpreter, name, address + prefix, bytes, 1);
        if (destination) {
            if (named(name, "strncpy")) {
                size_t copied = strlen(b) < bytes ? strlen(b) : bytes;
                memmove(destination, b, copied);
                memset(destination + copied, 0, bytes - copied);
            } else memmove(destination, b, bytes);
        }
        return (CtValue){.type = CHAR_POINTER, .as.address = address};
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
    if (named(name, "exit") || named(name, "srand") || named(name, "abs")) {
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
        return integer(named(name, "toupper") ? toupper(value) : tolower(value));
    }
    double a = real_number(interpreter, name, args[0]);
    double b = count == 2 ? real_number(interpreter, name, args[1]) : 0.0;
    if (interpreter->failed) return integer(0);
    double value = 0;
    errno = 0;
    if (named(name, "sqrt")) value = sqrt(a);
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
