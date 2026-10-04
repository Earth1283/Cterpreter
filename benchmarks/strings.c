#include <stdio.h>
#include <string.h>
#include <ctype.h>
int main(void) {
    char text[32]; int sum = 0;
    for (int i = 0; i < 40000; i++) {
        sprintf(text, "item-%06d", i);
        sum += (int)strlen(text);
        for (int j = 0; text[j]; j++) if (isdigit(text[j])) sum++;
    }
    printf("%d\n", sum);
    return 0;
}
