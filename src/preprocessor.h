#ifndef CT_PREPROCESSOR_H
#define CT_PREPROCESSOR_H

#include "cterpreter.h"

typedef struct Macro Macro;
struct Macro {
    char *name, *body;
    char **parameters;
    size_t count;
    int function, variadic;
    Macro *next;
};

typedef struct { Macro *macros; } Preprocessor;

int preprocess(Preprocessor *preprocessor, const char *source, const char *filename,
               char **output, CtError *error);
void preprocessor_destroy(Preprocessor *preprocessor);
int preprocessor_copy(Preprocessor *target, const Preprocessor *source);

#endif
