#ifndef CT_PREPROCESSOR_H
#define CT_PREPROCESSOR_H

#include "cterpreter.h"

typedef struct Macro Macro;
struct Macro {
    char *name, *body;
    size_t name_length;
    uint64_t hash;
    char **parameters;
    size_t count;
    int function, variadic;
    Macro *next;
    Macro *bucket_next;
};

#define PREPROCESSOR_BUCKETS 64

typedef struct {
    Macro *macros;
    Macro *buckets[PREPROCESSOR_BUCKETS];
    char **once;
    size_t once_count, counter;
    int initialized;
} Preprocessor;

int preprocess(Preprocessor *preprocessor, const char *source, const char *filename,
               char **output, CtError *error);
void preprocessor_destroy(Preprocessor *preprocessor);
int preprocessor_copy(Preprocessor *target, const Preprocessor *source);
const char *preprocessor_macro_name(const Preprocessor *preprocessor, size_t index);

#endif
