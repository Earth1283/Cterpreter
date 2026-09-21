#define _POSIX_C_SOURCE 200809L

#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const struct { const char *name, *description; } settings[] = {
    {"tips", "Plain-language syntax help"},
    {"highlighting", "Color C keywords, strings, numbers, and comments"},
    {"suggestions", "Inline suggestions and Tab completion"},
    {"signatures", "Function signatures while typing"},
    {"diagnostics", "Live parser diagnostics below the prompt"},
    {"color", "Terminal colors: auto, always, or never"}
};

void config_defaults(CliConfig *config) {
    for (int i = 0; i < CFG_COUNT; ++i) config->values[i] = i == CFG_COLOR ? 0 : 1;
}

int config_index(const char *name) {
    for (int i = 0; i < CFG_COUNT; ++i) if (!strcmp(name, settings[i].name)) return i;
    return -1;
}

const char *config_name(int index) { return settings[index].name; }
const char *config_description(int index) { return settings[index].description; }

const char *config_value(const CliConfig *config, int index) {
    if (index == CFG_COLOR) {
        static const char *colors[] = {"auto", "always", "never"};
        return colors[config->values[index]];
    }
    return config->values[index] ? "on" : "off";
}

int config_set(CliConfig *config, const char *name, const char *value) {
    int index = config_index(name);
    if (index < 0) return 0;
    if (index == CFG_COLOR) {
        if (!strcmp(value, "auto")) config->values[index] = 0;
        else if (!strcmp(value, "always")) config->values[index] = 1;
        else if (!strcmp(value, "never")) config->values[index] = 2;
        else return 0;
    } else {
        if (!strcmp(value, "on")) config->values[index] = 1;
        else if (!strcmp(value, "off")) config->values[index] = 0;
        else return 0;
    }
    return 1;
}

static char *trim(char *text) {
    while (isspace((unsigned char)*text)) ++text;
    size_t length = strlen(text);
    while (length && isspace((unsigned char)text[length - 1])) text[--length] = '\0';
    return text;
}

int config_load(CliConfig *config, const char *path, int optional) {
    FILE *file = fopen(path, "r");
    if (!file) {
        if (optional && errno == ENOENT) return 1;
        perror(path);
        return 0;
    }
    CliConfig pending = *config;
    char *line = NULL;
    size_t capacity = 0, number = 0;
    ssize_t size;
    int ok = 1;
    while ((size = getline(&line, &capacity, file)) >= 0) {
        ++number;
        if (size > 4096 || memchr(line, '\0', (size_t)size)) { ok = 0; break; }
        char *comment = strchr(line, '#');
        if (comment) *comment = '\0';
        char *name = trim(line);
        if (!*name) continue;
        char *value = strchr(name, '=');
        if (!value) { ok = 0; break; }
        *value++ = '\0';
        if (!config_set(&pending, trim(name), trim(value))) { ok = 0; break; }
    }
    if (!ok) fprintf(stderr, "%s:%zu: invalid setting; use name=on/off or color=auto/always/never\n", path, number);
    if (ferror(file)) { perror(path); ok = 0; }
    free(line);
    if (fclose(file) != 0) { perror(path); ok = 0; }
    if (ok) *config = pending;
    return ok;
}

int config_save(const CliConfig *config, const char *path) {
    size_t length = strlen(path);
    char *temporary = malloc(length + sizeof ".tmp.XXXXXX");
    if (!temporary) { fputs("error: out of memory\n", stderr); return 0; }
    memcpy(temporary, path, length);
    memcpy(temporary + length, ".tmp.XXXXXX", sizeof ".tmp.XXXXXX");
    int descriptor = mkstemp(temporary);
    FILE *file = descriptor < 0 ? NULL : fdopen(descriptor, "w");
    int ok = file != NULL;
    if (file) {
        if (fputs("# Cterpreter CLI preferences. Change these with .config.\n", file) == EOF) ok = 0;
        for (int i = 0; i < CFG_COUNT; ++i)
            if (fprintf(file, "%s=%s\n", config_name(i), config_value(config, i)) < 0) ok = 0;
        if (fclose(file) != 0) ok = 0;
        if (ok && rename(temporary, path) != 0) ok = 0;
    } else if (descriptor >= 0) close(descriptor);
    if (!ok) { perror(path); unlink(temporary); }
    free(temporary);
    return ok;
}
