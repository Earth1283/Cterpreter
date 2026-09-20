#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define MAX_WORDS 32

int main(void) {
    char text[] = "C makes pointers. Pointers make C interesting; C makes debugging interesting.";
    char *words[MAX_WORDS];
    int counts[MAX_WORDS] = {0};
    int unique = 0;
    char *cursor = text;

    while (*cursor) {
        while (*cursor && !isalpha(*cursor)) ++cursor;
        if (!*cursor) break;
        char *word = cursor;
        while (*cursor && isalpha(*cursor)) {
            *cursor = tolower(*cursor);
            ++cursor;
        }
        if (*cursor) *cursor++ = '\0';

        int index = 0;
        while (index < unique && strcmp(words[index], word) != 0) ++index;
        if (index == unique) {
            if (unique == MAX_WORDS) return 1;
            words[unique++] = word;
        }
        ++counts[index];
    }

    for (int i = 0; i < unique; ++i) {
        for (int j = i + 1; j < unique; ++j) {
            if (strcmp(words[i], words[j]) <= 0) continue;
            char *word = words[i];
            words[i] = words[j];
            words[j] = word;
            int count = counts[i];
            counts[i] = counts[j];
            counts[j] = count;
        }
    }
    for (int i = 0; i < unique; ++i) printf("%-12s %d\n", words[i], counts[i]);
    return 0;
}
