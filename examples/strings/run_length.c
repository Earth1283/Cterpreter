#include <ctype.h>
#include <stdio.h>
#include <string.h>

int encode(char *input, char *output, int capacity) {
    int used = 0;
    while (*input) {
        char letter = *input++;
        int count = 1;
        while (*input == letter) {
            ++count;
            ++input;
        }
        int written = snprintf(output + used, capacity - used, "%c%d", letter, count);
        if (written < 0 || written >= capacity - used) return -1;
        used += written;
    }
    output[used] = '\0';
    return used;
}

int decode(char *input, char *output, int capacity) {
    int used = 0;
    while (*input) {
        char letter = *input++;
        if (!isalpha(letter) || !isdigit(*input)) return -1;
        int count = 0;
        while (isdigit(*input)) {
            int digit = *input++ - '0';
            if (count > (capacity - 1 - digit) / 10) return -1;
            count = count * 10 + digit;
        }
        if (count == 0 || count >= capacity - used) return -1;
        for (int i = 0; i < count; ++i) output[used++] = letter;
    }
    output[used] = '\0';
    return used;
}

int main(void) {
    char input[] = "aaaabbbccdeeeee";
    char encoded[64];
    char decoded[64];
    if (encode(input, encoded, sizeof(encoded)) < 0) return 1;
    if (decode(encoded, decoded, sizeof(decoded)) < 0) return 1;

    printf("Original: %s\nEncoded:  %s\nDecoded:  %s\n", input, encoded, decoded);
    printf("Round trip: %s\n", strcmp(input, decoded) == 0 ? "OK" : "FAILED");
    return strcmp(input, decoded) != 0;
}
