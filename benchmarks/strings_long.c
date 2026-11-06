#include <stdio.h>
#include <string.h>
#include <ctype.h>
int main(void) {
    long long total = 0;
    char text[32];
    for (int i = 0; i < 300000; i++) {
        snprintf(text, sizeof text, "item-%06d", i);
        total += (long long)strlen(text);
        for (int j = 0; text[j]; j++)
            if (isdigit((unsigned char)text[j])) total++;
    }
    printf("%lld\n", total);
    return 0;
}
