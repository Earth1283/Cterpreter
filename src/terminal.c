#define _POSIX_C_SOURCE 200809L

#include "terminal.h"
#include "cterpreter.h"
#include "lexer.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

static char *duplicate(const char *text) {
    size_t size = strlen(text) + 1;
    char *result = malloc(size);
    if (result) memcpy(result, text, size);
    return result;
}

static void history_add(Terminal *terminal, const char *text) {
    if (!*text || (terminal->count && !strcmp(terminal->history[terminal->count - 1], text))) return;
    if (terminal->count == 1000) {
        free(terminal->history[0]);
        memmove(terminal->history, terminal->history + 1, 999 * sizeof *terminal->history);
        --terminal->count;
    }
    char **history = realloc(terminal->history, (terminal->count + 1) * sizeof *history);
    if (!history) return;
    terminal->history = history;
    char *entry = duplicate(text);
    if (entry) terminal->history[terminal->count++] = entry;
}

void terminal_init(Terminal *terminal, const char *history_path, int color) {
    *terminal = (Terminal){.history_path = history_path, .color = color, .highlighting = 1, .suggestions = 1};
    if (!history_path) return;
    FILE *file = fopen(history_path, "r");
    if (!file) return;
    char *line = NULL;
    size_t capacity = 0;
    ssize_t length;
    while ((length = getline(&line, &capacity, file)) >= 0) {
        if (length > (ssize_t)CT_SOURCE_LIMIT) break;
        while (length && (line[length - 1] == '\n' || line[length - 1] == '\r')) line[--length] = '\0';
        history_add(terminal, line);
    }
    free(line);
    fclose(file);
}

static const char *completion(Terminal *terminal, const char *line, size_t length) {
    if (!terminal->suggestions) return NULL;
    if (terminal->complete) {
        const char *suffix = terminal->complete(terminal->session, line, length);
        if (suffix) return suffix;
    }
    static const char *words[] = {
        ".help", ".config", ".quit", ".clear", ".version", ".type", ".source", ".vars", ".ast", ".load", ".save", ".restore", ".depth",
        "printf", "puts", "putchar", "getchar", "malloc", "calloc", "realloc", "free", "sizeof", "strlen", "strcmp",
        "strcpy", "memcpy", "memset", "snprintf", "return", "continue", "break", "double", "static", "const", "switch"
    };
    size_t start = length;
    while (start && (isalnum((unsigned char)line[start - 1]) || line[start - 1] == '_' || line[start - 1] == '.')) --start;
    size_t prefix = length - start;
    if (!prefix) return NULL;
    for (size_t i = 0; i < sizeof words / sizeof words[0]; ++i)
        if (strlen(words[i]) > prefix && !memcmp(line + start, words[i], prefix)) return words[i] + prefix;
    return NULL;
}

typedef struct { int comment, quote; } SyntaxState;

/* Tolerant tokenization: unfinished strings/comments still get useful colors. */
static size_t syntax_span(const char *text, size_t start, SyntaxState *state, const char **style) {
    size_t end = start;
    *style = "\033[0m";
    if (state->comment || (text[start] == '/' && text[start + 1] == '*')) {
        if (!state->comment) end += 2;
        state->comment = 1;
        while (text[end] && !(text[end] == '*' && text[end + 1] == '/')) ++end;
        if (text[end]) { end += 2; state->comment = 0; }
        *style = "\033[90m";
    } else if (text[start] == '/' && text[start + 1] == '/') {
        while (text[end] && text[end] != '\n') ++end;
        *style = "\033[90m";
    } else if (state->quote || text[start] == '"' || text[start] == '\'') {
        if (!state->quote) state->quote = text[end++];
        while (text[end]) {
            if (text[end] == '\\' && text[end + 1]) end += 2;
            else if (text[end++] == state->quote) { state->quote = 0; break; }
        }
        *style = "\033[32m";
    } else if (isalpha((unsigned char)text[start]) || text[start] == '_') {
        Lexer lexer;
        lexer_init(&lexer, text + start);
        Token token = lexer_next(&lexer);
        end += token.length;
        if (token.kind != TK_NAME) *style = "\033[1;35m";
        switch (token.kind) {
            case TK_INT: case TK_DOUBLE: case TK_VOID: case TK_CHAR: case TK_SIGNED:
            case TK_UNSIGNED: case TK_SHORT: case TK_LONG: case TK_FLOAT: case TK_BOOL:
            case TK_STRUCT: case TK_UNION: case TK_ENUM: case TK_TYPEDEF:
                *style = "\033[36m"; break;
            default: break;
        }
        size_t following = end;
        while (isspace((unsigned char)text[following])) ++following;
        if (token.kind == TK_NAME && text[following] == '(') *style = "\033[34m";
    } else if (isdigit((unsigned char)text[start]) || (text[start] == '.' && isdigit((unsigned char)text[start + 1]))) {
        ++end;
        while (isalnum((unsigned char)text[end]) || text[end] == '.' ||
               ((text[end] == '+' || text[end] == '-') && strchr("eEpP", text[end - 1]))) ++end;
        *style = "\033[33m";
    } else if (text[start] == '#' || (text[start] == '.' && !start)) {
        ++end;
        while (isalpha((unsigned char)text[end]) || text[end] == '_') ++end;
        *style = "\033[1;36m";
    } else ++end;
    return end;
}

static void plain_span(const char *text, size_t start, size_t end) {
    for (size_t i = start; i < end; ++i) {
        unsigned char c = (unsigned char)text[i];
        fputc(c >= 32 && c != 127 ? c : '?', stdout);
    }
}

static void highlight(Terminal *terminal, const char *line, size_t length, size_t start, size_t end) {
    if (!terminal->color || !terminal->highlighting) { plain_span(line, start, end); return; }
    SyntaxState state = {0};
    const char *style;
    if (terminal->context)
        for (size_t i = 0; terminal->context[i];) i = syntax_span(terminal->context, i, &state, &style);
    for (size_t i = 0; i < length;) {
        size_t next = syntax_span(line, i, &state, &style);
        if (next > start && i < end) {
            fputs(style, stdout);
            plain_span(line, i < start ? start : i, next < end ? next : end);
        }
        i = next;
        if (i >= end) break;
    }
    fputs("\033[0m", stdout);
}

static size_t render(Terminal *terminal, const char *prompt, const char *line, size_t length, size_t cursor, int status) {
    struct winsize window = {0};
    size_t columns = ioctl(STDOUT_FILENO, TIOCGWINSZ, &window) == 0 && window.ws_col >= 8 ? window.ws_col : 80;
    size_t prompt_length = strlen(prompt);
    if (prompt_length > columns / 2) prompt_length = columns / 2;
    size_t width = columns - prompt_length - 1;
    size_t start = cursor >= width ? cursor - width + 1 : 0;
    size_t end = length < start + width ? length : start + width;
    fputs("\r\033[2K", stdout);
    if (terminal->color) fputs("\033[1;36m", stdout);
    plain_span(prompt, 0, prompt_length);
    if (terminal->color) fputs("\033[0m", stdout);
    highlight(terminal, line, length, start, end);
    const char *suggestion = status && cursor == length ? completion(terminal, line, length) : NULL;
    if (suggestion && terminal->color) {
        size_t size = strlen(suggestion), available = width - (end - start);
        fputs("\033[90m", stdout);
        plain_span(suggestion, 0, size < available ? size : available);
        fputs("\033[0m", stdout);
    }
    const char *note = status && terminal->hint ? terminal->hint(terminal->session, line, cursor) : NULL;
    fputs("\n\033[2K", stdout);
    if (note) {
        if (terminal->color) fputs("\033[90m", stdout);
        size_t note_length = strlen(note);
        plain_span(note, 0, note_length < columns - 1 ? note_length : columns - 1);
        if (terminal->color) fputs("\033[0m", stdout);
    }
    fputs("\033[A\r", stdout);
    size_t column = prompt_length + cursor - start;
    if (column) fprintf(stdout, "\033[%zuC", column);
    fflush(stdout);
    return column;
}

static int read_byte(const volatile sig_atomic_t *interrupted) {
    unsigned char byte;
    while (!*interrupted) {
        ssize_t count = read(STDIN_FILENO, &byte, 1);
        if (count == 1) return byte;
        if (!count) return -1;
        if (errno != EINTR) return -1;
    }
    return -2;
}

static int escape_byte(const volatile sig_atomic_t *interrupted) {
    fd_set input;
    FD_ZERO(&input);
    FD_SET(STDIN_FILENO, &input);
    struct timeval timeout = {.tv_usec = 100000};
    if (select(STDIN_FILENO + 1, &input, NULL, NULL, &timeout) <= 0) return -1;
    return read_byte(interrupted);
}

char *terminal_read(Terminal *terminal, const char *prompt, const volatile sig_atomic_t *interrupted) {
    struct termios saved;
    if (tcgetattr(STDIN_FILENO, &saved) != 0) return NULL;
    struct termios raw = saved;
    raw.c_lflag &= (tcflag_t)~(ICANON | ECHO);
    raw.c_iflag &= (tcflag_t)~(IXON | ICRNL);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) return NULL;
    char *line = calloc(CT_SOURCE_LIMIT + 1, 1);
    size_t length = 0, cursor = 0, history = terminal->count;
    char *draft = NULL;
    int accepted = 0;
    if (line) render(terminal, prompt, line, length, cursor, 1);
    while (line && !*interrupted) {
        int byte = read_byte(interrupted);
        if (byte < 0) break;
        if (byte == '\n' || byte == '\r') { accepted = 1; break; }
        if (byte == 4 && !length) break;
        if (byte == 1) cursor = 0;
        else if (byte == 5) cursor = length;
        else if (byte == 2 && cursor) --cursor;
        else if (byte == 6 && cursor < length) ++cursor;
        else if (byte == 11) line[length = cursor] = '\0';
        else if (byte == 21) { memmove(line, line + cursor, length - cursor + 1); length -= cursor; cursor = 0; }
        else if (byte == 23) {
            size_t start = cursor;
            while (start && isspace((unsigned char)line[start - 1])) --start;
            while (start && !isspace((unsigned char)line[start - 1])) --start;
            memmove(line + start, line + cursor, length - cursor + 1);
            length -= cursor - start;
            cursor = start;
        } else if ((byte == 127 || byte == 8) && cursor) {
            memmove(line + cursor - 1, line + cursor, length - cursor + 1);
            --length; --cursor;
        } else if (byte == 4 && cursor < length) {
            memmove(line + cursor, line + cursor + 1, length - cursor);
            --length;
        } else if (byte == 9 && cursor == length) {
            const char *suffix = completion(terminal, line, length);
            if (suffix && strlen(suffix) <= CT_SOURCE_LIMIT - length) {
                strcpy(line + length, suffix);
                length += strlen(suffix);
                cursor = length;
            }
        } else if (byte == 18) {
            char *query = duplicate(line);
            if (query) {
                for (size_t i = history; i > 0; --i)
                    if (strstr(terminal->history[i - 1], query)) {
                        if (!draft) draft = duplicate(line);
                        history = i - 1;
                        strcpy(line, terminal->history[history]);
                        cursor = length = strlen(line);
                        break;
                    }
                free(query);
            }
        } else if (byte == 27) {
            int prefix = escape_byte(interrupted);
            if (prefix == '[' || prefix == 'O') {
                int key = escape_byte(interrupted);
                if ((key == 'A' || key == 'B') && terminal->count) {
                    if (!draft) draft = duplicate(line);
                    if (key == 'A' && history) --history;
                    if (key == 'B' && history < terminal->count) ++history;
                    const char *entry = history < terminal->count ? terminal->history[history] : draft ? draft : "";
                    strcpy(line, entry);
                    cursor = length = strlen(line);
                } else if (key == 'C' && cursor < length) ++cursor;
                else if (key == 'D' && cursor) --cursor;
                else if (key == 'H') cursor = 0;
                else if (key == 'F') cursor = length;
                else if (key >= '0' && key <= '9') {
                    int end = escape_byte(interrupted);
                    if (key == '3' && end == '~' && cursor < length) {
                        memmove(line + cursor, line + cursor + 1, length - cursor);
                        --length;
                    }
                }
            }
        } else if (byte >= 32 && byte != 127 && length < CT_SOURCE_LIMIT) {
            memmove(line + cursor + 1, line + cursor, length - cursor + 1);
            line[cursor++] = (char)byte;
            ++length;
        }
        render(terminal, prompt, line, length, cursor, 1);
    }
    (void)tcsetattr(STDIN_FILENO, TCSANOW, &saved);
    if (line) {
        size_t column = render(terminal, prompt, line, length, length, 0);
        fputs("\r\033[B\033[2K\033[A\r", stdout);
        if (column) fprintf(stdout, "\033[%zuC", column);
    }
    fputc('\n', stdout);
    free(draft);
    if (!accepted) { free(line); return NULL; }
    history_add(terminal, line);
    return line;
}

void terminal_destroy(Terminal *terminal) {
    if (terminal->history_path) {
        FILE *file = fopen(terminal->history_path, "w");
        if (file) {
            for (size_t i = 0; i < terminal->count; ++i) fprintf(file, "%s\n", terminal->history[i]);
            fclose(file);
        }
    }
    for (size_t i = 0; i < terminal->count; ++i) free(terminal->history[i]);
    free(terminal->history);
    *terminal = (Terminal){0};
}
