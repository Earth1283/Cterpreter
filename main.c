#include <stdio.h>
#include <string.h>

#define INPUT_SIZE 4096

int main(void) {
    char input[INPUT_SIZE];

    puts("Cterpreter 0.1.0");
    puts("Type .quit to exit from hell.");

    while (1) {
        printf("c> ");
        fflush(stdout);

        if (!fgets(input, sizeof(input), stdin))
            break;

        input[strcspn(input, "\n")] = '\0';

        if (strcmp(input, ".quit") == 0)
            break;

        if (input[0] == '\0')
            continue;

        printf("You entered: %s\n", input);
    }

    puts("You have escaped from hell.");
    return 0;
}