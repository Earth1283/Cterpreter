#include "cterpreter.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Malformed input must produce a diagnostic, never a crash and never a hang,
 * and must leave the session able to evaluate the next submission. */
static int feed(const char *source, size_t length) {
    CtInterpreter *interpreter = ct_create();
    if (!interpreter) return 0;
    char *text = malloc(length + 1);
    if (!text) { ct_destroy(interpreter); return 0; }
    memcpy(text, source, length);
    text[length] = '\0';
    ct_set_limits(interpreter, 20000, 64);
    CtValue value;
    CtError error = {0};
    int has_value = 0, ok = 1;
    CtStatus status = ct_eval(interpreter, text, &value, &has_value, &error);
    if (status != CT_OK && status != CT_INCOMPLETE && status != CT_ERROR) ok = 0;
    if (status != CT_OK && !error.message[0]) ok = 0;
    if (ct_eval(interpreter, "1 + 1", &value, &has_value, &error) != CT_OK ||
        !has_value || value.as.integer != 2) ok = 0;
    if (!ok) fprintf(stderr, "Failed on input: %.*s\n", (int)(length > 200 ? 200 : length), text);
    free(text);
    ct_destroy(interpreter);
    return ok;
}

#ifdef CT_LIBFUZZER
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size <= 4096 && !feed((const char *)data, size)) abort();
    return 0;
}
#else

static uint32_t state = 0x5eed1234u;

static uint32_t next_random(void) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

static const char *const fragments[] = {
    "int", "char", "double", "void", "struct", "union", "enum", "typedef", "static", "const",
    "if", "else", "while", "for", "do", "switch", "case", "default", "return", "break", "continue",
    "goto", "sizeof", "_Alignof", "_Generic", "printf", "malloc", "free", "main", "x", "y", "p",
    "{", "}", "(", ")", "[", "]", ";", ",", ".", "->", "*", "&", "+", "-", "/", "%", "=", "==",
    "<<=", "...", "?", ":", "!", "~", "^", "|", "&&", "||", "++", "--", "#define", "#if", "#endif",
    "#include", "\"text\"", "'c'", "0", "1", "42", "2147483648", "0x", "1e", "1.5", "\\", "@",
    "\"unterminated", "/*", "*/", "//", "\n", " ", "__FILE__", "__VA_ARGS__", "##",
    "__ct_va_start", "__ct_va_arg", "__ct_va_end", "__ct_va_copy", "va_list", "\"stdarg.h\""
};

static size_t build(char *buffer, size_t capacity) {
    size_t length = 0;
    unsigned pieces = next_random() % 24 + 1;
    for (unsigned i = 0; i < pieces; ++i) {
        const char *piece = fragments[next_random() % (sizeof fragments / sizeof fragments[0])];
        size_t size = strlen(piece);
        if (length + size + 2 >= capacity) break;
        memcpy(buffer + length, piece, size);
        length += size;
        buffer[length++] = next_random() % 4 ? ' ' : '\n';
    }
    return length;
}

int main(int argc, char **argv) {
    unsigned rounds = 2000;
    if (argc > 1) rounds = (unsigned)strtoul(argv[1], NULL, 10);
    if (argc > 2) state = (uint32_t)strtoul(argv[2], NULL, 10) | 1u;
    char buffer[1024];
    for (unsigned round = 0; round < rounds; ++round) {
        size_t length = build(buffer, sizeof buffer);
        if (!feed(buffer, length)) return EXIT_FAILURE;
    }
    /* Bytes that are not C at all still have to be refused politely. */
    for (unsigned round = 0; round < rounds / 4; ++round) {
        size_t length = next_random() % 128;
        for (size_t i = 0; i < length; ++i) buffer[i] = (char)(next_random() % 127 + 1);
        if (!feed(buffer, length)) return EXIT_FAILURE;
    }
    printf("fuzz: %u generated and %u random inputs refused without incident\n", rounds, rounds / 4);
    return EXIT_SUCCESS;
}
#endif
