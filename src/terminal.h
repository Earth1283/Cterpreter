#ifndef CT_TERMINAL_H
#define CT_TERMINAL_H

#include <signal.h>
#include <stddef.h>

typedef struct {
    char **history;
    size_t count, capacity;
    const char *history_path;
    int color, highlighting, suggestions;
    const char *context;
    /* Supplied by the host so that hints and completions come from the live session. */
    void *session;
    const char *(*hint)(void *session, const char *line, size_t cursor);
    const char *(*complete)(void *session, const char *line, size_t cursor);
} Terminal;

void terminal_init(Terminal *terminal, const char *history_path, int color);
char *terminal_read(Terminal *terminal, const char *prompt, const volatile sig_atomic_t *interrupted);
void terminal_destroy(Terminal *terminal);

#endif
