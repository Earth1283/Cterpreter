#ifndef CT_TYPES_H
#define CT_TYPES_H

#include "cterpreter.h"

typedef enum {
    TY_INT, TY_DOUBLE, TY_CHAR, TY_VOID, TY_POINTER, TY_ARRAY, TY_STRUCT, TY_UNION, TY_FUNCTION
} TypeKind;

typedef struct {
    char *name;
    size_t length;
    CtType type;
    size_t offset;
} Member;

typedef struct {
    TypeKind kind;
    CtType target;
    size_t count, size, align;
    char *tag;
    Member *members;
    size_t member_count;
    CtType *parameters;
    size_t parameter_count;
    int complete, variadic;
} TypeInfo;

const TypeInfo *type_info(CtType type);
TypeKind type_kind(CtType type);
size_t ct_type_align(CtType type);

CtType type_pointer(CtType target);
CtType type_array(CtType element, size_t count);
CtType type_function(CtType result, const CtType *parameters, size_t count, int variadic);
CtType type_aggregate(int is_union, const char *tag, size_t tag_length);
int type_add_member(CtType aggregate, const char *name, size_t length, CtType type);
int type_finish(CtType aggregate);
const Member *type_member(CtType aggregate, const char *name, size_t length);

/* The element type of an array or the referenced type of a pointer. */
CtType type_target(CtType type);
/* Arrays used in expressions yield a pointer to their first element. */
CtType type_decay(CtType type);

static inline int type_is_pointer(CtType type) { return type_kind(type) == TY_POINTER; }
static inline int type_is_array(CtType type) { return type_kind(type) == TY_ARRAY; }
static inline int type_is_function(CtType type) { return type_kind(type) == TY_FUNCTION; }
static inline int type_is_aggregate(CtType type) {
    TypeKind kind = type_kind(type);
    return kind == TY_STRUCT || kind == TY_UNION;
}
static inline int type_is_integer(CtType type) {
    TypeKind kind = type_kind(type);
    return kind == TY_INT || kind == TY_CHAR;
}
static inline int type_is_number(CtType type) {
    TypeKind kind = type_kind(type);
    return kind == TY_INT || kind == TY_CHAR || kind == TY_DOUBLE;
}
/* Objects that a CtValue carries directly rather than by address. */
static inline int type_is_scalar(CtType type) { return type_is_number(type) || type_is_pointer(type); }

void types_retain(void);
void types_release(void);

#endif
