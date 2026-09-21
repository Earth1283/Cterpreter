#include "cterpreter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void check(CtInterpreter *interpreter, const char *source, int expected, const char *diagnostic) {
    CtValue value = {0};
    CtError error = {0};
    int has_value = 0;
    CtStatus status = ct_eval(interpreter, source, &value, &has_value, &error);
    int ok = diagnostic ? status == CT_ERROR && strstr(error.message, diagnostic)
                        : status == CT_OK && has_value && value.type == CT_INT && value.as.integer == expected;
    if (!ok) {
        fprintf(stderr, "Failed: %s\nstatus=%d, value=%lld, diagnostic=%s\n",
                source, status, (long long)value.as.integer, error.message);
        exit(EXIT_FAILURE);
    }
    if (diagnostic) check(interpreter, "6 * 7", 42, NULL);
}

int main(void) {
    CtInterpreter *interpreter = ct_create();
    if (!interpreter) return EXIT_FAILURE;
    check(interpreter, "#include <stdarg.h>\n#include <stdarg.h>\n#include <stdio.h>\n"
        "int sum(int n, ...) { va_list ap; va_start(ap, n); int total = 0;"
        "for (int i = 0; i < n; ++i) total += va_arg(ap, int); va_end(ap); return total; }"
        "sum(4, (_Bool)1, (char)2, (short)3, 4)", 10, NULL);
    check(interpreter, "sum(0)", 0, NULL);
    check(interpreter, "int (*indirect)(int, ...) = sum; indirect(3, 10, 20, 12)", 42, NULL);
    check(interpreter, "sum()", 0, "number of arguments");
    check(interpreter, "sum(1)", 0, "no remaining argument");
    check(interpreter, "sum(1, 2.0)", 0, "promoted argument type");
    check(interpreter, "sum(2, 20, 22)", 42, NULL);
    check(interpreter, "double real(int n, ...) { va_list ap; va_start(ap, n);"
        "double x = va_arg(ap, double); va_end(ap); return x; } real(1, 1.25f) == 1.25", 1, NULL);
    check(interpreter, "int copies(int n, ...) { va_list a, b; va_start(a, n);"
        "int x = va_arg(a, int); va_copy(b, a); int y = va_arg(a, int); va_end(a);"
        "int z = va_arg(b, int); va_end(b); va_start(a, n); int first = va_arg(a, int); va_end(a);"
        "return x + y + z + first; } copies(2, 10, 11)", 42, NULL);
    check(interpreter, "int recursive(int n, ...) { va_list ap; va_start(ap, n);"
        "int x = va_arg(ap, int); if (n) x += recursive(n - 1, 2, 3);"
        "x += va_arg(ap, int); va_end(ap); return x; } recursive(3, 10, 17)", 42, NULL);
    check(interpreter, "int next_arg(va_list *p) { return va_arg(*p, int); }"
        "int helper(int n, ...) { va_list ap; va_start(ap, n); int a = next_arg(&ap);"
        "int b = va_arg(ap, int); va_end(ap); return a + b; } helper(2, 20, 22)", 42, NULL);
    check(interpreter, "int take_list(va_list ap) { return va_arg(ap, int); }"
        "int pass_list(int n, ...) { va_list ap; va_start(ap, n); int x = take_list(ap);"
        "va_end(ap); return x; } pass_list(1, 42)", 42, NULL);
    check(interpreter, "struct Pair { int x, y; }; struct Pair pair = {20, 22};"
        "int aggregate(int n, ...) { va_list ap, copy; va_start(ap, n); va_copy(copy, ap);"
        "pair.x = 99; struct Pair x = va_arg(ap, struct Pair); x.y = 0;"
        "struct Pair y = va_arg(copy, struct Pair); va_end(ap); va_end(copy); return x.x + y.y; }"
        "aggregate(1, pair)", 42, NULL);
    check(interpreter, "typedef int (*Callback)(int, ...);"
        "int pointer_args(int n, ...) { va_list ap; va_start(ap, n); int *p = va_arg(ap, int *);"
        "Callback f = va_arg(ap, Callback); int x = f(2, p[0], p[1]); va_end(ap); return x; }"
        "int numbers[2] = {20, 22}; pointer_args(2, numbers, sum)", 42, NULL);
    check(interpreter, "int unevaluated(int n, ...) { va_list ap; va_start(ap, n);"
        "int x = _Generic(va_arg(ap, int), int: 1, default: 0);"
        "x += sizeof(va_arg(ap, int)) == sizeof(int); x += va_arg(ap, int); va_end(ap); return x; }"
        "unevaluated(1, 40)", 42, NULL);
    check(interpreter, "unsigned opposite(int n, ...) { va_list ap; va_start(ap, n);"
        "unsigned x = va_arg(ap, unsigned); va_end(ap); return x; } opposite(1, 42) == 42u", 1, NULL);
    check(interpreter, "opposite(1, -1)", 0, "promoted argument type");
    check(interpreter, "sum(1, 42u)", 42, NULL);
    check(interpreter, "sum(1, 4294967295u)", 0, "promoted argument type");
    check(interpreter, "int void_char(int n, ...) { va_list ap; va_start(ap, n);"
        "void *a = va_arg(ap, void *); char *b = va_arg(ap, char *);"
        "va_end(ap); return a == b; } void_char(2, numbers, numbers)", 0, "promoted argument type");
    check(interpreter, "char text[] = \"ok\"; void_char(2, text, (void *)text)", 1, NULL);
    check(interpreter, "int evaluate_once(int n, ...) { va_list lists[2]; int i = 0;"
        "va_start(lists[i++], n); va_copy(lists[i++], lists[0]); i = 0;"
        "int a = va_arg(lists[i++], int); int b = va_arg(lists[i++], int);"
        "va_end(lists[0]); va_end(lists[1]); return a + b + i; } evaluate_once(1, 20)", 42, NULL);
    check(interpreter, "typedef int (*Row)[2]; int array_pointer(int n, ...) { va_list ap; va_start(ap, n);"
        "Row row = va_arg(ap, Row); va_end(ap); return (*row)[0] + (*row)[1]; } array_pointer(1, &numbers)", 42, NULL);
    check(interpreter, "int narrow(int n, ...) { va_list ap; va_start(ap, n); return va_arg(ap, char); }"
        "narrow(1, (char)1)", 0, "promoted argument type");
    check(interpreter, "int wrong_last(int n, int m, ...) { va_list ap; va_start(ap, n); return 0; }"
        "wrong_last(1, 2)", 0, "last named parameter");
    check(interpreter, "int shadow(int n, ...) { va_list ap; { int n = 1; va_start(ap, n); } return 0; }"
        "shadow(0)", 0, "last named parameter");
    check(interpreter, "void not_variadic(int n) { va_list ap; va_start(ap, n); } not_variadic(0)",
        0, "requires a variadic function");
    check(interpreter, "int call_nonvariadic(int n, ...) { not_variadic(0); return 0; } call_nonvariadic(0)",
        0, "requires a variadic function");
    check(interpreter, "va_list global; va_start(global, missing)", 0, "requires a variadic function");
    check(interpreter, "va_arg(global, int)", 0, "not active");
    check(interpreter, "int uninitialized(int n, ...) { va_list ap; return va_arg(ap, int); } uninitialized(1, 1)",
        0, "uninitialized");
    check(interpreter, "int ended(int n, ...) { va_list ap; va_start(ap, n); va_end(ap); return va_arg(ap, int); }"
        "ended(1, 1)", 0, "not active");
    check(interpreter, "int escape(int n, ...) { va_list ap; va_start(ap, n); va_copy(global, ap);"
        "va_end(ap); return 0; } escape(1, 42); va_arg(global, int)", 0, "returned function");
    check(interpreter, "sum(1, 42); va_arg(global, int)", 0, "returned function");
    check(interpreter, "va_copy(global, global)", 0, "returned function");
    check(interpreter, "va_arg(numbers, int)", 0, "requires a va_list");
    check(interpreter, "va_arg(global, void)", 0, "complete non-array");
    check(interpreter, "int recurse_forever(int n, ...) { va_list ap; va_start(ap, n);"
        "return recurse_forever(n, va_arg(ap, int)); } recurse_forever(1, 42)", 0, "nesting limit");
    check(interpreter, "sum(2, 20, 22)", 42, NULL);
    check(interpreter, "int no_void(int n, ...) { return n; } no_void(1, (void)0)",
        0, "void expression cannot be a variadic argument");

    check(interpreter, "int format(char *out, unsigned long size, const char *fmt, ...) {"
        "va_list ap; va_start(ap, fmt); int r = vsnprintf(out, size, fmt, ap); va_end(ap); return r; }"
        "char buffer[64]; format(buffer, sizeof(buffer), \"%s %*.*f %lld\", \"ok\", 5, 2, 1.5f, 42ll)", 11, NULL);
    check(interpreter, "strcmp(buffer, \"ok  1.50 42\")", 0, NULL);
    check(interpreter, "format(buffer, 4, \"%s\", \"abcdef\"); strcmp(buffer, \"abc\")", 0, NULL);
    check(interpreter, "format((char *)0, 0, \"hello %d\", 42)", 8, NULL);
    check(interpreter, "int twice_format(char *out, const char *fmt, ...) { va_list ap, copy;"
        "va_start(ap, fmt); va_copy(copy, ap); int n = vsnprintf((char *)0, 0, fmt, ap);"
        "va_end(ap); int m = vsprintf(out, fmt, copy); va_end(copy); return n == m; }"
        "twice_format(buffer, \"%d %.1f\", 42, 2.5)", 1, NULL);
    check(interpreter, "strcmp(buffer, \"42 2.5\")", 0, NULL);
    check(interpreter, "int prefix(int n, ...) { va_list ap; va_start(ap, n); int first = va_arg(ap, int);"
        "int r = vsnprintf(buffer, sizeof(buffer), \"%d\", ap); va_end(ap); return first + r; }"
        "prefix(2, 40, 42)", 42, NULL);
    check(interpreter, "strcmp(buffer, \"42\")", 0, NULL);
    check(interpreter, "int consumed(int n, ...) { va_list ap; va_start(ap, n);"
        "vsnprintf(buffer, sizeof(buffer), \"%d\", ap); return va_arg(ap, int); } consumed(2, 1, 2)",
        0, "consumed by formatted I/O");
    check(interpreter, "format(buffer, sizeof(buffer), \"%d\")", 0, "not enough");
    check(interpreter, "vprintf(\"x\")", 0, "number of library arguments");
    check(interpreter, "int scan(const char *s, const char *fmt, ...) { va_list ap; va_start(ap, fmt);"
        "int r = vsscanf(s, fmt, ap); va_end(ap); return r; } int a; double b; char word[8];"
        "scan(\"42 2.5 hello\", \"%d %lf %7s\", &a, &b, word)", 3, NULL);
    check(interpreter, "a == 42 && b == 2.5 && strcmp(word, \"hello\") == 0", 1, NULL);
    check(interpreter, "scan(\"42\", \"%d\")", 0, "not enough");

    FILE *input = tmpfile(), *output = tmpfile();
    if (!input || !output) return EXIT_FAILURE;
    fputs("20 22", input);
    rewind(input);
    ct_set_streams(interpreter, input, output, output);
    check(interpreter, "int read_stdin(int file, const char *fmt, ...) { va_list ap; va_start(ap, fmt);"
        "int r = file ? vfscanf(stdin, fmt, ap) : vscanf(fmt, ap); va_end(ap); return r; }"
        "read_stdin(0, \"%d\", &a); read_stdin(1, \"%d\", &numbers[0]); a + numbers[0]", 42, NULL);
    check(interpreter, "int print(int file, const char *fmt, ...) { va_list ap; va_start(ap, fmt);"
        "int r = file ? vfprintf(stdout, fmt, ap) : vprintf(fmt, ap); va_end(ap); return r; }"
        "print(0, \"%d \", 20); print(1, \"%d\\n\", 22)", 3, NULL);
    rewind(output);
    char printed[32];
    if (!fgets(printed, sizeof printed, output) || strcmp(printed, "20 22\n")) return EXIT_FAILURE;
    ct_set_streams(interpreter, NULL, NULL, NULL);
    fclose(input);
    fclose(output);
    ct_clear(interpreter);
    check(interpreter, "#include <stdarg.h>\nint f(int n, ...) { va_list ap; va_start(ap, n);"
        "int r = va_arg(ap, int); va_end(ap); return r; } f(1, 42)", 42, NULL);
    ct_destroy(interpreter);
    return EXIT_SUCCESS;
}
