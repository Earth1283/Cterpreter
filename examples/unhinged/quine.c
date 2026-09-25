#include <stdio.h>

/* This program prints itself. Diff its output against this file. */
static const char *self[] = {
    "#include <stdio.h>",
    "",
    "/* This program prints itself. Diff its output against this file. */",
    "static const char *self[] = {",
    "@",
    "};",
    "",
    "static void quoted(const char *line) {",
    "    putchar('\"');",
    "    for (; *line; line++) {",
    "        if (*line == '\"' || *line == '\\\\') putchar('\\\\');",
    "        putchar(*line);",
    "    }",
    "    puts(\"\\\",\");",
    "}",
    "",
    "int main(void) {",
    "    int count = (int)(sizeof self / sizeof self[0]);",
    "    for (int i = 0; i < count; i++) {",
    "        if (self[i][0] != '@') { puts(self[i]); continue; }",
    "        for (int j = 0; j < count; j++) {",
    "            fputs(\"    \", stdout);",
    "            quoted(self[j]);",
    "        }",
    "    }",
    "    return 0;",
    "}",
};

static void quoted(const char *line) {
    putchar('"');
    for (; *line; line++) {
        if (*line == '"' || *line == '\\') putchar('\\');
        putchar(*line);
    }
    puts("\",");
}

int main(void) {
    int count = (int)(sizeof self / sizeof self[0]);
    for (int i = 0; i < count; i++) {
        if (self[i][0] != '@') { puts(self[i]); continue; }
        for (int j = 0; j < count; j++) {
            fputs("    ", stdout);
            quoted(self[j]);
        }
    }
    return 0;
}
