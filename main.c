#define _POSIX_C_SOURCE 200809L

#include "cterpreter.h"
#include "boot.h"
#include "terminal.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t interrupted;

typedef struct { char *data; size_t length, capacity; } Buffer;
typedef enum { READ_OK, READ_EOF, READ_INTERRUPTED, READ_ERROR } ReadStatus;
typedef struct {
    CtInterpreter *interpreter;
    Buffer source;
    const char *prompt, *history_path;
    size_t steps;
    unsigned depth, nesting;
    int color, verbose, strict;
} Application;

static void handle_interrupt(int signal_number) {
    (void)signal_number;
    interrupted = 1;
}

static int append(Buffer *buffer, int character) {
    if (buffer->length >= CT_SOURCE_LIMIT) return 0;
    if (buffer->length + 1 >= buffer->capacity) {
        size_t capacity = buffer->capacity ? buffer->capacity * 2 : 256;
        if (capacity > CT_SOURCE_LIMIT + 1) capacity = CT_SOURCE_LIMIT + 1;
        char *data = realloc(buffer->data, capacity);
        if (!data) return 0;
        buffer->data = data;
        buffer->capacity = capacity;
    }
    buffer->data[buffer->length++] = (char)character;
    buffer->data[buffer->length] = '\0';
    return 1;
}

static int append_text(Buffer *buffer, const char *text) {
    while (*text) if (!append(buffer, (unsigned char)*text++)) return 0;
    return 1;
}

static void reset(Buffer *buffer) {
    buffer->length = 0;
    if (buffer->data) buffer->data[0] = '\0';
}

static ReadStatus read_input(FILE *stream, Buffer *buffer, int one_line) {
    for (;;) {
        if (interrupted) { clearerr(stream); return READ_INTERRUPTED; }
        errno = 0;
        int character = fgetc(stream);
        if (interrupted) { clearerr(stream); return READ_INTERRUPTED; }
        if (character == EOF) {
            if (ferror(stream)) {
                if (errno == EINTR) { clearerr(stream); continue; }
                perror("input");
                return READ_ERROR;
            }
            return READ_EOF;
        }
        if (!character) {
            fputs("error: input contains a NUL byte\n", stderr);
            return READ_ERROR;
        }
        if (!append(buffer, character)) {
            fputs("error: source limit exceeded or memory exhausted\n", stderr);
            return READ_ERROR;
        }
        if (one_line && character == '\n') return READ_OK;
    }
}

static void print_error(Application *app, const char *name, const CtError *error, const char *source) {
    int color = app->color && isatty(STDERR_FILENO);
    fflush(stdout);
    if (color) fputs("\033[31m", stderr);
    fprintf(stderr, "%s:%zu:%zu: error: %s\n", name, error->line, error->column, error->message);
    if (color) fputs("\033[0m", stderr);
    if (!source || strchr(source, '#')) return;
    const char *line = source;
    for (size_t number = 1; number < error->line && *line; ++number) {
        const char *next = strchr(line, '\n');
        if (!next) return;
        line = next + 1;
    }
    const char *end = strchr(line, '\n');
    size_t length = end ? (size_t)(end - line) : strlen(line);
    if (length > 200 || !length) return;
    fprintf(stderr, "  %.*s\n  ", (int)length, line);
    for (size_t i = 1; i < error->column && i <= length; ++i) fputc(line[i - 1] == '\t' ? '\t' : ' ', stderr);
    fputs("^\n", stderr);
}

static CtStatus evaluate(Application *app, const char *source, const char *name, int final, int remember) {
    CtValue value;
    int has_value;
    CtError error;
    ct_set_filename(app->interpreter, name);
    CtStatus status = ct_eval(app->interpreter, source, &value, &has_value, &error);
    ct_set_filename(app->interpreter, NULL);
    if (status == CT_ERROR || (status == CT_INCOMPLETE && final)) print_error(app, name, &error, source);
    if (status == CT_OK && has_value) {
        char formatted[512];
        ct_print_value(app->interpreter, value, formatted, sizeof formatted);
        puts(formatted);
    }
    if (status == CT_OK && remember && *source) {
        size_t previous_length = app->source.length;
        if (!append_text(&app->source, source) || !append_text(&app->source, "\n;\n")) {
            app->source.length = previous_length;
            if (app->source.data) app->source.data[previous_length] = '\0';
            fputs("error: session source limit reached; latest submission was not recorded\n", stderr);
        }
    }
    return status;
}

static int load_file(const char *path, Buffer *buffer) {
    FILE *file = fopen(path, "rb");
    if (!file) { perror(path); return 0; }
    ReadStatus status = read_input(file, buffer, 0);
    fclose(file);
    return status == READ_EOF;
}

static CtInterpreter *create_interpreter(Application *app) {
    CtInterpreter *interpreter = ct_create();
    if (interpreter) {
        ct_set_interrupt(interpreter, &interrupted);
        ct_set_limits(interpreter, app->steps, app->depth);
        ct_set_nesting(interpreter, 0, app->nesting);
        ct_set_strict(interpreter, app->strict);
    }
    return interpreter;
}

static int command(Application *app, char *source) {
    while (isspace((unsigned char)*source)) ++source;
    if (*source != '.' || isdigit((unsigned char)source[1])) return 0;
    size_t length = strlen(source);
    while (length && isspace((unsigned char)source[length - 1])) source[--length] = '\0';
    char *argument = source;
    while (*argument && !isspace((unsigned char)*argument)) ++argument;
    if (*argument) *argument++ = '\0';
    while (isspace((unsigned char)*argument)) ++argument;
    if (!strcmp(source, ".quit")) return 2;
    if (!strcmp(source, ".version")) puts("Cterpreter " CT_VERSION);
    else if (!strcmp(source, ".clear")) {
        ct_clear(app->interpreter);
        reset(&app->source);
        puts("Session cleared.");
    } else if (!strcmp(source, ".help")) {
        puts("C source: expressions, declarations, functions, pointers, arrays, and standard I/O.\n"
             "End a statement with ';' to suppress its expression result.\n\n"
             ".help              Show help\n"
             ".quit              Exit\n"
             ".clear             Reset variables, functions, macros, and memory\n"
             ".version           Show version\n"
             ".vars              Inspect globals, functions, and memory usage\n"
             ".type EXPR         Inspect an expression's type without executing it\n"
             ".ast SOURCE        Show the parsed syntax tree\n"
             ".source            Show accepted session source\n"
             ".load FILE         Execute source in the current session\n"
             ".save FILE         Save session source for replay\n"
             ".restore FILE      Replay source into a fresh session\n"
             ".depth             Show the interpreter's own nesting level\n\n"
             "Arrows edit or recall history. Ctrl+R searches history using the current text.\n"
             "Tab accepts a suggestion drawn from the session's own names.\n"
             "The line below the prompt shows the signature of the call you are inside,\n"
             "or the first diagnostic in what you have typed so far.\n"
             "Ctrl+C cancels input or execution. Ctrl+D exits.");
    } else if (!strcmp(source, ".vars") || !strcmp(source, ".dump")) ct_dump(app->interpreter, stdout);
    else if (!strcmp(source, ".depth")) printf("Interpreter nesting level %u\n", ct_depth(app->interpreter));
    else if (!strcmp(source, ".source")) fputs(app->source.data ? app->source.data : "", stdout);
    else if (!strcmp(source, ".type")) {
        CtType type;
        CtError error;
        if (ct_inspect_type(app->interpreter, argument, &type, &error) == CT_OK) {
            char name[64];
            ct_type_name(type, name, sizeof name);
            puts(name);
        } else print_error(app, "<type>", &error, argument);
    } else if (!strcmp(source, ".ast")) {
        CtError error;
        if (ct_dump_ast(app->interpreter, argument, stdout, &error) != CT_OK) print_error(app, "<ast>", &error, argument);
    } else if (!strcmp(source, ".load") || !strcmp(source, ".restore") || !strcmp(source, ".save")) {
        size_t size = strlen(argument);
        if (size >= 2 && argument[0] == '"' && argument[size - 1] == '"') { argument[size - 1] = '\0'; ++argument; }
        if (!*argument) { fputs("error: command requires a file path\n", stderr); return 1; }
        if (!strcmp(source, ".save")) {
            FILE *file = fopen(argument, "wb");
            if (!file) perror(argument);
            else {
                int ok = fwrite(app->source.data ? app->source.data : "", 1, app->source.length, file) == app->source.length;
                if (fclose(file) != 0) ok = 0;
                if (!ok) perror(argument);
                else printf("Saved session source to %s\n", argument);
            }
        } else {
            Buffer buffer = {0};
            if (load_file(argument, &buffer)) {
                if (!strcmp(source, ".restore")) {
                    CtInterpreter *previous = app->interpreter;
                    app->interpreter = create_interpreter(app);
                    if (!app->interpreter) { app->interpreter = previous; fputs("error: out of memory\n", stderr); }
                    else if (evaluate(app, buffer.data ? buffer.data : "", argument, 1, 0) == CT_OK) {
                        ct_destroy(previous);
                        reset(&app->source);
                        (void)append_text(&app->source, buffer.data ? buffer.data : "");
                        puts("Session source replayed.");
                    } else { ct_destroy(app->interpreter); app->interpreter = previous; }
                } else (void)evaluate(app, buffer.data ? buffer.data : "", argument, 1, 1);
            }
            free(buffer.data);
        }
    } else fprintf(stderr, "error: unknown command '%s' (try .help)\n", source);
    return 1;
}

/* The identifier naming the call the cursor sits inside, if there is one. */
static size_t enclosing_call(const char *line, size_t cursor, size_t *length) {
    size_t depth = 0;
    for (size_t i = cursor; i > 0; --i) {
        char c = line[i - 1];
        if (c == ')') ++depth;
        else if (c == '(') {
            if (depth) { --depth; continue; }
            size_t end = i - 1;
            while (end && isspace((unsigned char)line[end - 1])) --end;
            size_t start = end;
            while (start && (isalnum((unsigned char)line[start - 1]) || line[start - 1] == '_')) --start;
            if (start == end || isdigit((unsigned char)line[start])) return SIZE_MAX;
            *length = end - start;
            return start;
        }
    }
    return SIZE_MAX;
}

static const char *session_hint(void *session, const char *line, size_t cursor) {
    static char note[256];
    Application *app = session;
    size_t length = 0;
    size_t start = enclosing_call(line, cursor, &length);
    if (start != SIZE_MAX && ct_signature(app->interpreter, line + start, length, note, sizeof note)) return note;
    while (*line && isspace((unsigned char)*line)) ++line;
    if (!*line || *line == '.' || !strchr(line, ';')) return NULL;
    CtError error;
    if (ct_check(app->interpreter, line, &error) != CT_ERROR) return NULL;
    (void)snprintf(note, sizeof note, "%zu:%zu: %s", error.line, error.column, error.message);
    return note;
}

static const char *session_completion(void *session, const char *line, size_t cursor) {
    static char name[128];
    Application *app = session;
    size_t start = cursor;
    while (start && (isalnum((unsigned char)line[start - 1]) || line[start - 1] == '_')) --start;
    size_t prefix = cursor - start;
    if (!prefix || isdigit((unsigned char)line[start])) return NULL;
    if (start && (line[start - 1] == '.' || line[start - 1] == '>')) return NULL;
    if (!ct_complete(app->interpreter, line + start, prefix, 0, name, sizeof name)) return NULL;
    return name + prefix;
}

static int repl(Application *app, int prompts) {
    Buffer buffer = {0};
    Terminal terminal;
    int editing = prompts && isatty(STDIN_FILENO) && isatty(STDOUT_FILENO) &&
                  (!getenv("TERM") || strcmp(getenv("TERM"), "dumb"));
    terminal_init(&terminal, editing ? app->history_path : NULL, app->color);
    terminal.session = app;
    terminal.hint = session_hint;
    terminal.complete = session_completion;
    if (prompts) {
        if (app->color) fputs("\033[1;36m", stdout);
        printf("Cterpreter %s", CT_VERSION);
        if (app->color) fputs("\033[0m", stdout);
        puts("  •  C, interpreted\n.help for commands · Ctrl+D to exit");
    }
    int result = 0;
    for (;;) {
        size_t previous_length = buffer.length;
        ReadStatus read_status;
        if (editing) {
            char *line = terminal_read(&terminal, buffer.length ? "... " : app->prompt, &interrupted);
            if (interrupted) read_status = READ_INTERRUPTED;
            else if (!line) read_status = READ_EOF;
            else read_status = append_text(&buffer, line) && append(&buffer, '\n') ? READ_OK : READ_ERROR;
            free(line);
        } else {
            if (prompts) { fputs(buffer.length ? "... " : app->prompt, stdout); fflush(stdout); }
            read_status = read_input(stdin, &buffer, 1);
        }
        if (read_status == READ_INTERRUPTED) {
            interrupted = 0;
            reset(&buffer);
            if (prompts && !editing) putchar('\n');
            continue;
        }
        if (read_status == READ_ERROR) { result = 1; break; }
        if (read_status == READ_EOF && !buffer.length) break;
        int handled = command(app, buffer.data + previous_length);
        if (handled == 2) break;
        if (handled) { reset(&buffer); continue; }
        CtStatus status = evaluate(app, buffer.data, "<stdin>", read_status == READ_EOF, 1);
        if (ct_exit_status(app->interpreter, &result)) break;
        if (status != CT_INCOMPLETE) reset(&buffer);
        interrupted = 0;
        if (read_status == READ_EOF) break;
    }
    terminal_destroy(&terminal);
    free(buffer.data);
    return result;
}

static void usage(FILE *stream) {
    fputs("Usage: Cterpreter [options] [file.c [arguments...]]\n"
          "       Cterpreter -e 'C source'\n\n"
          "  -e SOURCE         Execute source directly\n"
          "  --no-prompt       Read a quiet, line-oriented REPL\n"
          "  --prompt TEXT     Set the primary prompt\n"
          "  --color MODE      auto, always, or never\n"
          "  --history FILE    Choose a history file\n"
          "  --no-history      Disable persistent history\n"
          "  --max-steps N     Set the execution step limit\n"
          "  --max-depth N     Set the evaluation depth limit (up to 8192)\n"
          "  --max-nesting N   Set how deep interpret() may nest (up to 64)\n"
          "  --strict          Raise SIGSEGV on an invalid access instead of diagnosing it\n"
          "  --verbose         Show initialization details\n"
          "  --version         Show version\n"
          "  -h, --help        Show help\n\n"
          "A file runs main when present. '-' reads source from standard input.\n"
          "A final expression without ';' prints its value.\n", stream);
}

static int positive(const char *text, size_t maximum, size_t *result) {
    char *end;
    errno = 0;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno || !*text || *text == '-' || *end || !value || value > maximum) return 0;
    *result = (size_t)value;
    return 1;
}

int main(int argc, char **argv) {
    Application app = {.prompt = "c> ", .steps = 1000000, .depth = 2048, .nesting = 8};
    const char *path = NULL, *source = NULL, *color_mode = "auto";
    int quiet_repl = 0, path_index = 0, no_history = 0;
    for (int i = 1; i < argc; ++i) {
        const char *argument = argv[i];
        if (!strcmp(argument, "--help") || !strcmp(argument, "-h")) { usage(stdout); return 0; }
        if (!strcmp(argument, "--version")) { puts("Cterpreter " CT_VERSION); return 0; }
        if (!strcmp(argument, "--no-prompt")) quiet_repl = 1;
        else if (!strcmp(argument, "--no-history")) no_history = 1;
        else if (!strcmp(argument, "--verbose")) app.verbose = 1;
        else if (!strcmp(argument, "--strict")) app.strict = 1;
        else if ((!strcmp(argument, "-e") || !strcmp(argument, "--prompt") || !strcmp(argument, "--color") ||
                  !strcmp(argument, "--history") || !strcmp(argument, "--max-steps") ||
                  !strcmp(argument, "--max-depth") || !strcmp(argument, "--max-nesting")) && i + 1 < argc) {
            const char *value = argv[++i];
            if (!strcmp(argument, "-e")) { if (source) { usage(stderr); return 2; } source = value; }
            else if (!strcmp(argument, "--prompt")) app.prompt = value;
            else if (!strcmp(argument, "--color")) color_mode = value;
            else if (!strcmp(argument, "--history")) app.history_path = value;
            else {
                size_t limit;
                size_t maximum = !strcmp(argument, "--max-depth") ? 8192 : !strcmp(argument, "--max-nesting") ? 64 : SIZE_MAX;
                if (!positive(value, maximum, &limit)) { fputs("error: invalid execution limit\n", stderr); return 2; }
                if (!strcmp(argument, "--max-depth")) app.depth = (unsigned)limit;
                else if (!strcmp(argument, "--max-nesting")) app.nesting = (unsigned)limit;
                else app.steps = limit;
            }
        } else if (!strcmp(argument, "--") && i + 1 < argc) { path_index = ++i; path = argv[i]; break; }
        else if (argument[0] != '-' || !strcmp(argument, "-")) { path_index = i; path = argument; break; }
        else { usage(stderr); return 2; }
    }
    if ((quiet_repl && (path || source)) || (path && source)) { usage(stderr); return 2; }
    if (strcmp(color_mode, "auto") && strcmp(color_mode, "always") && strcmp(color_mode, "never")) {
        fputs("error: --color expects auto, always, or never\n", stderr); return 2;
    }
    app.color = !strcmp(color_mode, "always") || (!strcmp(color_mode, "auto") && isatty(STDOUT_FILENO) && !getenv("NO_COLOR"));
    char default_history[4096];
    if (!app.history_path && !no_history && getenv("HOME")) {
        int length = snprintf(default_history, sizeof default_history, "%s/.cterpreter_history", getenv("HOME"));
        if (length > 0 && (size_t)length < sizeof default_history) app.history_path = default_history;
    }
    if (no_history) app.history_path = NULL;
    struct sigaction action = {0};
    action.sa_handler = handle_interrupt;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) != 0) { perror("sigaction"); return 1; }
    app.interpreter = create_interpreter(&app);
    if (!app.interpreter) { fputs("error: out of memory\n", stderr); return 1; }
    ct_set_limits(app.interpreter, 0, 0);
    CtValue sanity;
    CtError diagnostic;
    int has_value;
    BootCheck check = boot_verify();
    if (!check.agreed || ct_eval(app.interpreter, "2 + 2", &sanity, &has_value, &diagnostic) != CT_OK ||
        !has_value || sanity.type != CT_INT || sanity.as.integer != 4) {
        fputs("FATAL: Arithmetic has abandoned us. Refusing to enter the void.\n", stderr);
        ct_destroy(app.interpreter);
        return 1;
    }
    ct_set_limits(app.interpreter, app.steps, app.depth);
    if (app.verbose)
        fprintf(stderr, "[PASS] 2 + 2 = 4, confirmed on %s\n[BOOT] The integers have been consulted.\n"
                        "[BOOT] A nop was executed ceremonially.\n[BOOT] %zu steps; depth %u; nesting %u.\n",
                check.unit, app.steps, app.depth, app.nesting);
    int status = 0;
    if (source) status = evaluate(&app, source, "<command>", 1, 0) == CT_OK ? 0 : 1;
    else if (!path && (quiet_repl || isatty(STDIN_FILENO))) status = repl(&app, !quiet_repl);
    else {
        Buffer buffer = {0};
        int read_ok = !path || !strcmp(path, "-") ? read_input(stdin, &buffer, 0) == READ_EOF : load_file(path, &buffer);
        if (!read_ok) status = 1;
        else if (evaluate(&app, buffer.data ? buffer.data : "", path ? path : "<stdin>", 1, 0) != CT_OK) status = 1;
        else if (ct_exit_status(app.interpreter, &status)) {}
        else if (ct_has_function(app.interpreter, "main")) {
            ct_set_filename(app.interpreter, path ? path : "<stdin>");
            const char *stdin_name[] = {"<stdin>"};
            const char *const *arguments = path ? (const char *const *)(argv + path_index) : stdin_name;
            int arguments_count = path ? argc - path_index : 1;
            if (ct_run_main(app.interpreter, arguments_count, arguments, &status, &diagnostic) != CT_OK) {
                print_error(&app, path ? path : "<stdin>", &diagnostic, buffer.data);
                status = 1;
            }
        }
        free(buffer.data);
    }
    if (source && !status) (void)ct_exit_status(app.interpreter, &status);
    ct_destroy(app.interpreter);
    free(app.source.data);
    return interrupted ? 130 : status;
}
