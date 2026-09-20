#include "cterpreter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static CtStatus run(CtInterpreter *interpreter, const char *source, CtValue *value) {
    CtError error;
    int has_value = 0;
    CtStatus status = ct_eval(interpreter, source, value, &has_value, &error);
    if (status == CT_OK && !has_value) value->type = CT_VOID;
    return status;
}

static void expect_value(CtInterpreter *interpreter, const char *source, CtType type, int64_t expected) {
    CtValue value;
    CtStatus status = run(interpreter, source, &value);
    if (status != CT_OK || value.type != type || value.as.integer != expected) {
        char wanted[64], got[64];
        ct_type_name(type, wanted, sizeof wanted);
        ct_type_name(value.type, got, sizeof got);
        fprintf(stderr, "Failed: %s -> status %d, %s %lld, expected %s %lld\n",
                source, status, got, (long long)value.as.integer, wanted, (long long)expected);
        ++failures;
    }
}

static void expect_int(CtInterpreter *interpreter, const char *source, int64_t expected) {
    expect_value(interpreter, source, CT_INT, expected);
}

static void expect_char(CtInterpreter *interpreter, const char *source, int64_t expected) {
    expect_value(interpreter, source, CT_CHAR, expected);
}

static void expect_real_of(CtInterpreter *interpreter, const char *source, CtType type, double expected) {
    CtValue value;
    CtStatus status = run(interpreter, source, &value);
    if (status != CT_OK || value.type != type || value.as.real != expected) {
        fprintf(stderr, "Failed: %s -> status %d, value %g, expected %g\n",
                source, status, value.as.real, expected);
        ++failures;
    }
}

static void expect_real(CtInterpreter *interpreter, const char *source, double expected) {
    CtValue value;
    CtStatus status = run(interpreter, source, &value);
    if (status != CT_OK || value.type != CT_DOUBLE || value.as.real != expected) {
        fprintf(stderr, "Failed: %s -> status %d, value %g, expected %g\n",
                source, status, value.as.real, expected);
        ++failures;
    }
}

/* Cterpreter refuses undefined arithmetic rather than wrapping quietly. */
static void expect_refused(CtInterpreter *interpreter, const char *source) {
    CtValue value;
    if (run(interpreter, source, &value) != CT_ERROR) {
        fprintf(stderr, "Failed: %s was accepted, expected a diagnostic\n", source);
        ++failures;
    }
}

static void expect_type(CtInterpreter *interpreter, const char *source, const char *expected) {
    CtType type;
    CtError error;
    char name[128];
    if (ct_inspect_type(interpreter, source, &type, &error) != CT_OK) {
        fprintf(stderr, "Failed: .type %s -> %s\n", source, error.message);
        ++failures;
        return;
    }
    ct_type_name(type, name, sizeof name);
    if (strcmp(name, expected)) {
        fprintf(stderr, "Failed: .type %s -> %s, expected %s\n", source, name, expected);
        ++failures;
    }
}

int main(void) {
    CtInterpreter *interpreter = ct_create();
    if (!interpreter) return EXIT_FAILURE;

    expect_int(interpreter, "2147483647", 2147483647);
    expect_int(interpreter, "-2147483647 - 1", -2147483648);
    expect_int(interpreter, "2147483647 - 1", 2147483646);
    expect_refused(interpreter, "2147483647 + 1");
    expect_refused(interpreter, "-2147483647 - 2");
    expect_refused(interpreter, "2147483647 * 2");
    expect_refused(interpreter, "(-2147483647 - 1) / -1");
    expect_refused(interpreter, "-(-2147483647 - 1)");
    expect_value(interpreter, "2147483647 + 1L", CT_LONG, 2147483648);
    expect_refused(interpreter, "1 / 0");
    expect_refused(interpreter, "1 % 0");

    expect_int(interpreter, "-7 / 2", -3);
    expect_int(interpreter, "-7 % 2", -1);
    expect_int(interpreter, "7 / -2", -3);
    expect_int(interpreter, "7 % -2", 1);

    expect_int(interpreter, "1 << 30", 1073741824);
    expect_int(interpreter, "-8 >> 1", -4);
    expect_int(interpreter, "-1 & 255", 255);
    expect_int(interpreter, "~0", -1);
    expect_int(interpreter, "5 ^ 3", 6);
    expect_refused(interpreter, "1 << 31");
    expect_refused(interpreter, "1 << 32");
    expect_refused(interpreter, "1 << -1");
    expect_refused(interpreter, "-1 << 1");

    expect_int(interpreter, "7 / 2", 3);
    expect_real(interpreter, "7.0 / 2", 3.5);
    expect_real(interpreter, "7 / 2.0", 3.5);
    expect_int(interpreter, "(int)3.9", 3);
    expect_int(interpreter, "(int)-3.9", -3);
    expect_int(interpreter, "(int)2147483647.0", 2147483647);
    expect_refused(interpreter, "(int)1e18");
    expect_refused(interpreter, "(int)-1e18");
    expect_real(interpreter, "(double)7", 7.0);
    expect_int(interpreter, "1.5 < 2", 1);
    expect_int(interpreter, "2.0 == 2", 1);

    expect_char(interpreter, "(char)65", 65);
    expect_char(interpreter, "(char)321", 65);
    expect_int(interpreter, "'a' + 1", 98);
    expect_int(interpreter, "-'a'", -97);
    expect_type(interpreter, "'a'", "int");
    expect_type(interpreter, "(char)1 + (char)1", "int");
    expect_type(interpreter, "1 + 1.0", "double");
    expect_type(interpreter, "1 < 2", "int");

    expect_int(interpreter, "int quotient = 7; quotient /= 2; quotient", 3);
    expect_char(interpreter, "char small = 300; small", 44);
    expect_int(interpreter, "double scaled = 5; scaled /= 2; (int)(scaled * 2)", 5);
    expect_refused(interpreter, "int overflowing = 2147483647; overflowing += 1; overflowing");

    expect_int(interpreter, "int values[4]; int *cursor = values; (int)((cursor + 3) - cursor)", 3);
    expect_type(interpreter, "values", "int[4]");
    expect_type(interpreter, "&values", "int (*)[4]");
    expect_type(interpreter, "values + 1", "int *");
    expect_refused(interpreter, "cursor + 5");
    expect_refused(interpreter, "cursor - 1");
    expect_int(interpreter, "int anchor = 5; int *at = &anchor; (int)at == (int)&anchor", 1);
    expect_refused(interpreter, "int implicit = at;");

    expect_int(interpreter, "1u - 2u == 4294967295u", 1);
    expect_int(interpreter, "-1 < 1u", 0);
    expect_int(interpreter, "(unsigned char)255 + 1", 256);
    expect_int(interpreter, "(unsigned)4294967295 + 1u == 0", 1);
    expect_int(interpreter, "(short)32767 + 1", 32768);
    expect_int(interpreter, "(char)-1 == -1 || (char)-1 == 255", 1);
    expect_int(interpreter, "1u << 31 != 0", 1);
    expect_int(interpreter, "-1 >> 1 == -1", 1);
    expect_int(interpreter, "(unsigned)-1 >> 1 == 2147483647u", 1);
    expect_int(interpreter, "9223372036854775807LL / 3LL > 0", 1);
    expect_refused(interpreter, "9223372036854775807LL + 1LL");
    expect_int(interpreter, "(signed char)127 + (signed char)1 == (signed char)-128", 0);
    expect_int(interpreter, "(int)((signed char)(127 + 1))", -128);
    expect_value(interpreter, "_Bool flag = 3; flag", CT_BOOL, 1);
    expect_type(interpreter, "1 + 1u", "unsigned int");
    expect_type(interpreter, "1u + 1L", "long");
    expect_type(interpreter, "1u + 1UL", "unsigned long");
    expect_type(interpreter, "(char)1 + (char)1", "int");
    expect_type(interpreter, "sizeof(int)", "unsigned long");
    expect_type(interpreter, "1.0f + 1.0f", "float");
    expect_type(interpreter, "1.0f + 1.0", "double");
    expect_type(interpreter, "1 << 1L", "int");
    expect_real_of(interpreter, "0.5f + 0.25f", CT_FLOAT, 0.75);
    expect_int(interpreter, "(int)sizeof(long long) * 8", 64);

    expect_int(interpreter, "enum Size { SMALL = 1, LARGE = 2147483647 }; LARGE", 2147483647);
    expect_refused(interpreter, "enum Bad { EDGE = 2147483647, PAST };");
    expect_int(interpreter, "int scaled_array[SMALL + 1]; (int)sizeof(scaled_array)", 2 * (int)sizeof(int));

    ct_destroy(interpreter);
    if (failures) fprintf(stderr, "%d arithmetic checks failed\n", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
