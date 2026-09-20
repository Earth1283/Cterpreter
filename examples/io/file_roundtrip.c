#include <errno.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: file_roundtrip.c NEW_FILE\n");
        return 2;
    }
    FILE *file = fopen(argv[1], "w+x");
    if (!file) {
        fprintf(stderr, "Could not create file: %s\n", strerror(errno));
        return 1;
    }

    int values[] = {8, 13, 21, 34, 55};
    int restored[5];
    int success = 0;
    int checksum = 0;

    if (fwrite(values, sizeof(int), 5, file) != 5) goto cleanup;
    if (fflush(file) != 0) goto cleanup;
    if (fseek(file, 0, SEEK_SET) != 0) goto cleanup;
    if (fread(restored, sizeof(int), 5, file) != 5) goto cleanup;
    if (memcmp(values, restored, sizeof(values)) != 0) goto cleanup;

    for (int i = 0; i < 5; ++i) checksum += restored[i];
    printf("Restored 5 integers; checksum=%d\n", checksum);
    success = 1;

cleanup:
    if (fclose(file) != 0) success = 0;
    if (remove(argv[1]) != 0) success = 0;
    return success ? 0 : 1;
}
