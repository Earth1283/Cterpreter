#include "preprocessor.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PP_DEPTH 64

typedef struct { char *data; size_t length, capacity; } Text;
typedef struct {
    Preprocessor *preprocessor;
    CtError *error;
    const char *file;
    size_t line;
    int failed;
} Expansion;

static char *copy(const char *source, size_t length) {
    char *result = malloc(length + 1);
    if (result) { memcpy(result, source, length); result[length] = '\0'; }
    return result;
}

static int append(Text *text, const char *source, size_t length) {
    if (length > CT_SOURCE_LIMIT || text->length > CT_SOURCE_LIMIT - length) return 0;
    size_t needed = text->length + length + 1;
    if (needed > text->capacity) {
        size_t capacity = needed < 128 ? 128 : needed * 2;
        char *data = realloc(text->data, capacity);
        if (!data) return 0;
        text->data = data;
        text->capacity = capacity;
    }
    memcpy(text->data + text->length, source, length);
    text->length += length;
    text->data[text->length] = '\0';
    return 1;
}

static void fail(Expansion *expansion, const char *message) {
    if (expansion->failed) return;
    expansion->failed = 1;
    *expansion->error = (CtError){.line = expansion->line, .column = 1};
    (void)snprintf(expansion->error->message, sizeof expansion->error->message, "%s", message);
}

static void emit(Expansion *expansion, Text *text, const char *source, size_t length) {
    if (!expansion->failed && !append(text, source, length)) fail(expansion, "preprocessor output limit or memory exhausted");
}

static const char *spaces(const char *text) { while (*text && isspace((unsigned char)*text)) ++text; return text; }
static int identifier(int c) { return isalnum((unsigned char)c) || c == '_'; }
static const char *name_end(const char *text) { while (identifier(*text)) ++text; return text; }

static Macro *find(Preprocessor *preprocessor, const char *name, size_t length) {
    for (Macro *macro = preprocessor->macros; macro; macro = macro->next)
        if (strlen(macro->name) == length && !memcmp(macro->name, name, length)) return macro;
    return NULL;
}

static void macro_destroy(Macro *macro) {
    free(macro->name);
    free(macro->body);
    for (size_t i = 0; i < macro->count; ++i) free(macro->parameters[i]);
    free(macro->parameters);
    free(macro);
}

static void undefine(Preprocessor *preprocessor, const char *name, size_t length) {
    Macro **link = &preprocessor->macros;
    while (*link) {
        Macro *macro = *link;
        if (strlen(macro->name) == length && !memcmp(macro->name, name, length)) {
            *link = macro->next;
            macro_destroy(macro);
            return;
        }
        link = &macro->next;
    }
}

void preprocessor_destroy(Preprocessor *preprocessor) {
    while (preprocessor->macros) {
        Macro *next = preprocessor->macros->next;
        macro_destroy(preprocessor->macros);
        preprocessor->macros = next;
    }
}

int preprocessor_copy(Preprocessor *target, const Preprocessor *source) {
    *target = (Preprocessor){0};
    for (Macro *macro = source->macros; macro; macro = macro->next) {
        Macro *item = calloc(1, sizeof *item);
        if (!item) goto failed;
        item->next = target->macros;
        target->macros = item;
        item->name = copy(macro->name, strlen(macro->name));
        item->body = copy(macro->body, strlen(macro->body));
        item->function = macro->function;
        item->variadic = macro->variadic;
        item->parameters = macro->count ? calloc(macro->count, sizeof *item->parameters) : NULL;
        if (!item->name || !item->body || (macro->count && !item->parameters)) goto failed;
        for (size_t i = 0; i < macro->count; ++i) {
            item->parameters[item->count++] = copy(macro->parameters[i], strlen(macro->parameters[i]));
            if (!item->parameters[i]) goto failed;
        }
    }
    return 1;
failed:
    preprocessor_destroy(target);
    return 0;
}

static const char *quoted_end(const char *text) {
    char quote = *text++;
    while (*text && *text != quote) {
        if (*text == '\\' && text[1]) ++text;
        ++text;
    }
    return *text ? text + 1 : text;
}

static int parameter_index(Macro *macro, const char *name, size_t length) {
    for (size_t i = 0; i < macro->count; ++i)
        if (strlen(macro->parameters[i]) == length && !memcmp(macro->parameters[i], name, length)) return (int)i;
    return -1;
}

static void expand(Expansion *expansion, const char *source, Text *result, Macro **disabled, size_t depth);

static void stringify(Expansion *expansion, Text *result, const char *source) {
    emit(expansion, result, "\"", 1);
    source = spaces(source);
    int whitespace = 0;
    while (*source) {
        if (isspace((unsigned char)*source)) { whitespace = 1; ++source; continue; }
        if (whitespace) { emit(expansion, result, " ", 1); whitespace = 0; }
        if (*source == '"' || *source == '\\') emit(expansion, result, "\\", 1);
        emit(expansion, result, source++, 1);
    }
    emit(expansion, result, "\"", 1);
}

static void substitute(Expansion *expansion, Macro *macro, char **args, Text *result, Macro **disabled, size_t depth) {
    Text replacement = {0};
    const char *cursor = macro->body;
    while (*cursor && !expansion->failed) {
        if (*cursor == '"' || *cursor == '\'') {
            const char *end = quoted_end(cursor);
            emit(expansion, &replacement, cursor, (size_t)(end - cursor));
            cursor = end;
        } else if (cursor[0] == '#' && cursor[1] == '#') {
            while (replacement.length && isspace((unsigned char)replacement.data[replacement.length - 1])) --replacement.length;
            if (replacement.data) replacement.data[replacement.length] = '\0';
            cursor = spaces(cursor + 2);
        } else if (*cursor == '#') {
            cursor = spaces(cursor + 1);
            const char *end = name_end(cursor);
            int parameter = parameter_index(macro, cursor, (size_t)(end - cursor));
            if (parameter < 0) { fail(expansion, "# requires a macro parameter"); break; }
            stringify(expansion, &replacement, args[parameter]);
            cursor = end;
        } else if (isalpha((unsigned char)*cursor) || *cursor == '_') {
            const char *end = name_end(cursor);
            int parameter = parameter_index(macro, cursor, (size_t)(end - cursor));
            if (parameter >= 0) {
                const char *after = spaces(end), *before = cursor;
                while (before > macro->body && isspace((unsigned char)before[-1])) --before;
                int paste = (after[0] == '#' && after[1] == '#') || (before > macro->body && before[-1] == '#');
                if (paste) {
                    const char *raw = spaces(args[parameter]);
                    size_t length = strlen(raw);
                    while (length && isspace((unsigned char)raw[length - 1])) --length;
                    emit(expansion, &replacement, raw, length);
                } else {
                    expand(expansion, args[parameter], &replacement, disabled, depth - 1);
                    disabled[depth - 1] = macro;
                }
            } else emit(expansion, &replacement, cursor, (size_t)(end - cursor));
            cursor = end;
        } else emit(expansion, &replacement, cursor++, 1);
    }
    if (!expansion->failed) expand(expansion, replacement.data ? replacement.data : "", result, disabled, depth);
    free(replacement.data);
}

static void expand(Expansion *expansion, const char *source, Text *result, Macro **disabled, size_t depth) {
    if (depth >= PP_DEPTH) { fail(expansion, "macro expansion depth exceeded"); return; }
    for (const char *cursor = source; *cursor && !expansion->failed;) {
        if (*cursor == '"' || *cursor == '\'') {
            const char *end = quoted_end(cursor);
            emit(expansion, result, cursor, (size_t)(end - cursor));
            cursor = end;
            continue;
        }
        if (isdigit((unsigned char)*cursor) || (*cursor == '.' && isdigit((unsigned char)cursor[1]))) {
            const char *end = cursor + 1;
            while (identifier(*end) || *end == '.' || ((*end == '+' || *end == '-') && strchr("eEpP", end[-1]))) ++end;
            emit(expansion, result, cursor, (size_t)(end - cursor));
            cursor = end;
            continue;
        }
        if (!isalpha((unsigned char)*cursor) && *cursor != '_') {
            if (*cursor == '\n' && !depth) ++expansion->line;
            emit(expansion, result, cursor++, 1);
            continue;
        }
        const char *end = name_end(cursor);
        size_t length = (size_t)(end - cursor);
        if (length == 8 && !memcmp(cursor, "__LINE__", 8)) {
            char line[32];
            (void)snprintf(line, sizeof line, "%zu", expansion->line);
            emit(expansion, result, line, strlen(line));
            cursor = end;
            continue;
        }
        if (length == 8 && !memcmp(cursor, "__FILE__", 8)) {
            stringify(expansion, result, expansion->file);
            cursor = end;
            continue;
        }
        Macro *macro = find(expansion->preprocessor, cursor, length);
        for (size_t i = 0; macro && i < depth; ++i) if (disabled[i] == macro) macro = NULL;
        const char *after = spaces(end);
        if (!macro || (macro->function && *after != '(')) {
            emit(expansion, result, cursor, length);
            cursor = end;
            continue;
        }
        disabled[depth] = macro;
        if (!macro->function) {
            expand(expansion, macro->body, result, disabled, depth + 1);
            cursor = end;
            continue;
        }
        char **args = calloc(macro->count + 1, sizeof *args);
        if (!args) { fail(expansion, "out of memory"); break; }
        size_t count = 0;
        const char *start = after + 1, *scan = start;
        unsigned nesting = 0;
        for (;;) {
            if (!*scan) { fail(expansion, "unterminated macro invocation"); break; }
            if (*scan == '"' || *scan == '\'') { scan = quoted_end(scan); continue; }
            int at_end = *scan == ')' && !nesting;
            int at_comma = *scan == ',' && !nesting && !(macro->variadic && count + 1 == macro->count);
            if (at_end || at_comma) {
                if (count >= macro->count + 1) { fail(expansion, "too many macro arguments"); break; }
                args[count] = copy(start, (size_t)(scan - start));
                if (!args[count++]) { fail(expansion, "out of memory"); break; }
                if (at_end) { ++scan; break; }
                start = ++scan;
                continue;
            }
            if (*scan == '(') ++nesting;
            if (*scan == ')') --nesting;
            ++scan;
        }
        if (!macro->count && count == 1 && !*spaces(args[0])) { free(args[0]); args[0] = NULL; count = 0; }
        if (count != macro->count) fail(expansion, "incorrect number of macro arguments");
        if (!expansion->failed) substitute(expansion, macro, args, result, disabled, depth + 1);
        for (size_t i = 0; i < count; ++i) free(args[i]);
        free(args);
        cursor = scan;
    }
}

static void define(Expansion *expansion, const char *source) {
    source = spaces(source);
    const char *end = name_end(source);
    if (end == source || isdigit((unsigned char)*source)) { fail(expansion, "invalid macro name"); return; }
    Macro *macro = calloc(1, sizeof *macro);
    if (!macro) { fail(expansion, "out of memory"); return; }
    macro->name = copy(source, (size_t)(end - source));
    if (*end == '(') {
        macro->function = 1;
        source = spaces(end + 1);
        while (*source && *source != ')' && !expansion->failed) {
            int variadic = !strncmp(source, "...", 3);
            end = variadic ? source + 3 : name_end(source);
            if (end == source) { fail(expansion, "invalid macro parameter"); break; }
            char **parameters = realloc(macro->parameters, (macro->count + 1) * sizeof *parameters);
            if (!parameters) { fail(expansion, "out of memory"); break; }
            macro->parameters = parameters;
            macro->parameters[macro->count] = variadic ? copy("__VA_ARGS__", 11) : copy(source, (size_t)(end - source));
            if (!macro->parameters[macro->count++]) { fail(expansion, "out of memory"); break; }
            macro->variadic = variadic;
            source = spaces(end);
            if (*source != ',' || variadic) break;
            source = spaces(source + 1);
        }
        if (*source != ')') fail(expansion, "expected ')' after macro parameters");
        end = *source ? source + 1 : source;
    }
    end = spaces(end);
    macro->body = copy(end, strlen(end));
    if (!macro->name || !macro->body) fail(expansion, "out of memory");
    if (expansion->failed) { macro_destroy(macro); return; }
    undefine(expansion->preprocessor, macro->name, strlen(macro->name));
    macro->next = expansion->preprocessor->macros;
    expansion->preprocessor->macros = macro;
}

static int condition(Expansion *expansion, const char *source) {
    Text defined = {0}, expanded = {0}, numeric = {0};
    for (const char *cursor = source; *cursor && !expansion->failed;) {
        if (!strncmp(cursor, "defined", 7) && !identifier(cursor[7])) {
            cursor = spaces(cursor + 7);
            int paren = *cursor == '(';
            if (paren) cursor = spaces(cursor + 1);
            const char *end = name_end(cursor);
            int exists = find(expansion->preprocessor, cursor, (size_t)(end - cursor)) != NULL;
            emit(expansion, &defined, exists ? "1" : "0", 1);
            cursor = spaces(end);
            if (paren) {
                if (*cursor != ')') { fail(expansion, "expected ')' after defined"); break; }
                ++cursor;
            }
        } else if (isalpha((unsigned char)*cursor) || *cursor == '_') {
            const char *end = name_end(cursor);
            emit(expansion, &defined, cursor, (size_t)(end - cursor));
            cursor = end;
        } else emit(expansion, &defined, cursor++, 1);
    }
    Macro *disabled[PP_DEPTH] = {0};
    if (!expansion->failed) expand(expansion, defined.data ? defined.data : "", &expanded, disabled, 0);
    for (const char *cursor = expanded.data ? expanded.data : ""; *cursor;) {
        if (*cursor == '\'') {
            const char *end = quoted_end(cursor);
            emit(expansion, &numeric, cursor, (size_t)(end - cursor));
            cursor = end;
        } else if (isdigit((unsigned char)*cursor)) {
            const char *end = name_end(cursor);
            emit(expansion, &numeric, cursor, (size_t)(end - cursor));
            cursor = end;
        } else if (isalpha((unsigned char)*cursor) || *cursor == '_') {
            cursor = name_end(cursor);
            emit(expansion, &numeric, "0", 1);
        } else emit(expansion, &numeric, cursor++, 1);
    }
    int result = 0;
    if (!expansion->failed) {
        CtInterpreter *interpreter = ct_create();
        CtValue value;
        CtError error;
        int has_value;
        if (!interpreter) fail(expansion, "out of memory");
        else {
            CtStatus status = ct_eval(interpreter, numeric.data ? numeric.data : "", &value, &has_value, &error);
            if (status != CT_OK || !has_value || value.type != CT_INT) fail(expansion, "invalid #if expression");
            else result = value.as.integer != 0;
            ct_destroy(interpreter);
        }
    }
    free(defined.data); free(expanded.data); free(numeric.data);
    return result;
}

static char *clean_source(Expansion *expansion, const char *source) {
    Text spliced = {0}, clean = {0};
    for (const char *cursor = source; *cursor;) {
        if (cursor[0] == '\\' && cursor[1] == '\n') cursor += 2;
        else if (cursor[0] == '\\' && cursor[1] == '\r' && cursor[2] == '\n') cursor += 3;
        else emit(expansion, &spliced, cursor++, 1);
    }
    for (const char *cursor = spliced.data ? spliced.data : ""; *cursor && !expansion->failed;) {
        if (*cursor == '"' || *cursor == '\'') {
            const char *end = quoted_end(cursor);
            emit(expansion, &clean, cursor, (size_t)(end - cursor));
            cursor = end;
        } else if (cursor[0] == '/' && cursor[1] == '/') {
            while (*cursor && *cursor != '\n') ++cursor;
            emit(expansion, &clean, " ", 1);
        } else if (cursor[0] == '/' && cursor[1] == '*') {
            cursor += 2;
            emit(expansion, &clean, " ", 1);
            while (*cursor && !(cursor[0] == '*' && cursor[1] == '/')) {
                if (*cursor == '\n') emit(expansion, &clean, "\n", 1);
                ++cursor;
            }
            if (!*cursor) { fail(expansion, "unterminated block comment"); break; }
            cursor += 2;
        } else emit(expansion, &clean, cursor++, 1);
    }
    free(spliced.data);
    if (!clean.data) clean.data = copy("", 0);
    return clean.data;
}

static int process(Expansion *expansion, const char *source, Text *output, unsigned depth);

static void include(Expansion *expansion, const char *source, Text *output, unsigned depth) {
    Text expanded = {0};
    Macro *disabled[PP_DEPTH] = {0};
    expand(expansion, source, &expanded, disabled, 0);
    const char *start = spaces(expanded.data ? expanded.data : "");
    int system = *start == '<';
    char close = system ? '>' : '"';
    if (*start != '<' && *start != '"') { fail(expansion, "include requires a header name"); free(expanded.data); return; }
    ++start;
    const char *end = strchr(start, close);
    if (!end) { fail(expansion, "unterminated include name"); free(expanded.data); return; }
    char *header = copy(start, (size_t)(end - start));
    free(expanded.data);
    if (!header) { fail(expansion, "out of memory"); return; }
    static const char *headers[] = {"stdio.h", "stdlib.h", "string.h", "math.h", "ctype.h", "stddef.h", "limits.h", "time.h", "errno.h"};
    int builtin = 0;
    for (size_t i = 0; i < sizeof headers / sizeof headers[0]; ++i) if (!strcmp(header, headers[i])) builtin = 1;
    if (system && builtin) {
        char constants[512];
        (void)snprintf(constants, sizeof constants, "#define NULL 0\n#define EOF -1\n#define INT_MAX %d\n#define INT_MIN (-%d - 1)\n#define CHAR_BIT %d\n#define size_t int\n#define FILE void\n#define SEEK_SET 0\n#define SEEK_CUR 1\n#define SEEK_END 2\n#define RAND_MAX 32767\n", INT_MAX, INT_MAX, CHAR_BIT);
        size_t line = expansion->line;
        Text declarations = {0};
        (void)process(expansion, constants, &declarations, depth + 1);
        free(declarations.data);
        expansion->line = line;
        free(header);
        return;
    }
    if (system) { fail(expansion, "standard header is not implemented"); free(header); return; }
    char filename[4096];
    const char *slash = strrchr(expansion->file, '/');
    size_t directory = slash ? (size_t)(slash - expansion->file + 1) : 0;
    if (header[0] == '/') directory = 0;
    if (directory + strlen(header) >= sizeof filename) { fail(expansion, "include path is too long"); free(header); return; }
    memcpy(filename, expansion->file, directory);
    strcpy(filename + directory, header);
    free(header);
    FILE *file = fopen(filename, "rb");
    if (!file) { fail(expansion, "could not open included file"); return; }
    Text content = {0};
    char buffer[4096];
    size_t bytes;
    while ((bytes = fread(buffer, 1, sizeof buffer, file)) != 0) emit(expansion, &content, buffer, bytes);
    if (ferror(file)) fail(expansion, "could not read included file");
    fclose(file);
    const char *parent_file = expansion->file;
    size_t parent_line = expansion->line;
    expansion->file = filename;
    if (!expansion->failed) (void)process(expansion, content.data ? content.data : "", output, depth + 1);
    expansion->file = parent_file;
    expansion->line = parent_line;
    free(content.data);
}

static int process(Expansion *expansion, const char *source, Text *output, unsigned depth) {
    if (depth > 32) { fail(expansion, "include nesting limit exceeded"); return 0; }
    char *clean = clean_source(expansion, source);
    if (!clean) { fail(expansion, "out of memory"); return 0; }
    struct { int parent, taken, active, seen_else; } conditions[64];
    size_t nesting = 0;
    int active = 1;
    Text pending = {0};
    size_t pending_line = 1;
    expansion->line = 1;
    for (char *line = clean; *line && !expansion->failed;) {
        char *newline = strchr(line, '\n');
        if (newline) *newline = '\0';
        const char *cursor = spaces(line);
        if (*cursor == '#') {
            if (pending.length) {
                Macro *disabled[PP_DEPTH] = {0};
                size_t current_line = expansion->line;
                expansion->line = pending_line;
                expand(expansion, pending.data, output, disabled, 0);
                expansion->line = current_line;
                pending.length = 0;
                pending.data[0] = '\0';
            }
            cursor = spaces(cursor + 1);
            const char *end = name_end(cursor);
            size_t length = (size_t)(end - cursor);
            const char *body = spaces(end);
            int ifdef = length == 5 && !memcmp(cursor, "ifdef", 5);
            int ifndef = length == 6 && !memcmp(cursor, "ifndef", 6);
            if ((length == 2 && !memcmp(cursor, "if", 2)) || ifdef || ifndef) {
                if (nesting == 64) { fail(expansion, "conditional nesting limit exceeded"); break; }
                int value = 0;
                if (active) {
                    if (ifdef || ifndef) {
                        value = find(expansion->preprocessor, body, (size_t)(name_end(body) - body)) != NULL;
                        if (ifndef) value = !value;
                    } else value = condition(expansion, body);
                }
                conditions[nesting].parent = active;
                conditions[nesting].active = active && value;
                conditions[nesting].taken = value;
                conditions[nesting].seen_else = 0;
                active = conditions[nesting++].active;
            } else if ((length == 4 && !memcmp(cursor, "else", 4)) || (length == 4 && !memcmp(cursor, "elif", 4))) {
                if (!nesting || conditions[nesting - 1].seen_else) { fail(expansion, "unexpected conditional branch"); break; }
                size_t index = nesting - 1;
                int is_else = !memcmp(cursor, "else", 4);
                int value = 0;
                if (conditions[index].parent && !conditions[index].taken) value = is_else || condition(expansion, body);
                active = conditions[index].parent && value;
                conditions[index].active = active;
                conditions[index].taken |= value;
                conditions[index].seen_else = is_else;
            } else if (length == 5 && !memcmp(cursor, "endif", 5)) {
                if (!nesting) { fail(expansion, "unexpected #endif"); break; }
                active = conditions[--nesting].parent;
            } else if (active) {
                if (length == 6 && !memcmp(cursor, "define", 6)) define(expansion, body);
                else if (length == 5 && !memcmp(cursor, "undef", 5)) undefine(expansion->preprocessor, body, (size_t)(name_end(body) - body));
                else if (length == 7 && !memcmp(cursor, "include", 7)) include(expansion, body, output, depth);
                else if (length == 5 && !memcmp(cursor, "error", 5)) fail(expansion, body);
                else if (length == 6 && !memcmp(cursor, "pragma", 6)) fail(expansion, "pragma is not implemented; use include guards");
                else if (length) fail(expansion, "unsupported preprocessor directive");
            }
            emit(expansion, output, "\n", 1);
        } else {
            if (!pending.length) pending_line = expansion->line;
            if (active) emit(expansion, &pending, line, strlen(line));
            emit(expansion, &pending, "\n", 1);
        }
        if (!newline) break;
        line = newline + 1;
        ++expansion->line;
    }
    if (nesting) fail(expansion, "unterminated conditional directive");
    if (pending.length && !expansion->failed) {
        Macro *disabled[PP_DEPTH] = {0};
        expansion->line = pending_line;
        expand(expansion, pending.data, output, disabled, 0);
    }
    free(pending.data);
    free(clean);
    return !expansion->failed;
}

int preprocess(Preprocessor *preprocessor, const char *source, const char *filename, char **output, CtError *error) {
    Expansion expansion = {.preprocessor = preprocessor, .error = error, .file = filename, .line = 1};
    Text result = {0};
    int ok = process(&expansion, source, &result, 0);
    if (ok && !result.data) result.data = copy("", 0);
    if (ok && !result.data) { fail(&expansion, "out of memory"); ok = 0; }
    *output = ok ? result.data : NULL;
    if (!ok) free(result.data);
    return ok;
}
