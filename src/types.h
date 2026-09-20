#ifndef CT_TYPES_H
#define CT_TYPES_H

#include "cterpreter.h"

/* The scalar kinds are listed in the same order as the CT_* handles so that a
 * basic type's handle is its kind. */
typedef enum {
    TY_BOOL, TY_CHAR, TY_SCHAR, TY_UCHAR, TY_SHORT, TY_USHORT, TY_INT, TY_UINT,
    TY_LONG, TY_ULONG, TY_LLONG, TY_ULLONG, TY_FLOAT, TY_DOUBLE, TY_VOID,
    TY_POINTER, TY_ARRAY, TY_STRUCT, TY_UNION, TY_FUNCTION
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
    int is_signed, rank;
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
static inline int type_is_integer(CtType type) { return type_kind(type) <= TY_ULLONG; }
static inline int type_is_real(CtType type) {
    TypeKind kind = type_kind(type);
    return kind == TY_FLOAT || kind == TY_DOUBLE;
}
static inline int type_is_number(CtType type) { return type_kind(type) <= TY_DOUBLE; }
static inline int type_is_signed(CtType type) { return type_info(type)->is_signed; }

/* C17 6.3.1.1: anything narrower than int becomes int in an expression. */
CtType type_promote(CtType type);
/* C17 6.3.1.8: the type the two operands of an arithmetic operator share. */
CtType type_common(CtType left, CtType right);
/* The widest value the type can hold, for range checks and wrapping. */
uint64_t type_mask(CtType type);
int64_t type_minimum(CtType type);
int64_t type_maximum(CtType type);
/* Objects that a CtValue carries directly rather than by address. */
static inline int type_is_scalar(CtType type) { return type_is_number(type) || type_is_pointer(type); }

void types_retain(void);
void types_release(void);

#endif
