#include <stdio.h>

#define MAX_ACCOUNTS 16

int main(int argc, char **argv) {
    char *path = argc > 1 ? argv[1] : "examples/io/fixtures/transactions.csv";
    FILE *input = fopen(path, "r");
    if (!input) {
        fprintf(stderr, "Could not open %s.\n", path);
        return 1;
    }

    int accounts[MAX_ACCOUNTS];
    double totals[MAX_ACCOUNTS] = {0};
    int unique = 0;
    int rows = 0;
    char line[128];
    if (!fgets(line, sizeof(line), input)) {
        fclose(input);
        return 1;
    }

    while (fgets(line, sizeof(line), input)) {
        int account;
        double amount;
        if (sscanf(line, "%d,%lf", &account, &amount) != 2) {
            fprintf(stderr, "Invalid transaction at line %d.\n", rows + 2);
            fclose(input);
            return 1;
        }
        int index = 0;
        while (index < unique && accounts[index] != account) ++index;
        if (index == unique) {
            if (unique == MAX_ACCOUNTS) {
                fprintf(stderr, "Too many accounts.\n");
                fclose(input);
                return 1;
            }
            accounts[unique++] = account;
        }
        totals[index] += amount;
        ++rows;
    }
    int failed = ferror(input);
    fclose(input);
    if (failed) return 1;

    double total = 0.0;
    for (int i = 0; i < unique; ++i) {
        printf("Account %d: %7.2f\n", accounts[i], totals[i]);
        total += totals[i];
    }
    printf("%d transactions, %d accounts, total %.2f\n", rows, unique, total);
    return 0;
}
