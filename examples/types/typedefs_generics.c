#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define TYPE_NAME(value) _Generic((value), int: "int", double: "double", char: "char", char*: "char*", default: "other")
#define MAGNITUDE(value) _Generic((value), int: abs(value), double: fabs(value))

typedef int Count;
typedef double Measurement;
typedef char *Text;

enum Reading { LOW = 1, NORMAL, HIGH = 8 };

int next_id(void) {
    static int id = 100;
    return ++id;
}

int main(void) {
    Count count = -7;
    Measurement value = -3.5;
    Text label = "sensor";
    char unit = 'C';
    enum Reading reading = NORMAL;
    int first = next_id();
    int second = next_id();

    printf("%s: %s %.1f (%s)\n", label, TYPE_NAME(value), MAGNITUDE(value), TYPE_NAME(label));
    printf("count: %s %d; unit: %s %c\n", TYPE_NAME(count), MAGNITUDE(count), TYPE_NAME(unit), unit);
    printf("reading=%d ids=%d,%d\n", reading, first, second);
    printf("measurement size=%d alignment=%d\n", (int)sizeof(Measurement), (int)_Alignof(Measurement));
    return 0;
}
