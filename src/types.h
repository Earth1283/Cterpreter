#ifndef CT_TYPES_H
#define CT_TYPES_H

#include "cterpreter.h"

#include <limits.h>

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
    uint64_t mask;
    int64_t minimum, maximum;
    char *tag;
    Member *members;
    size_t member_count, member_capacity;
    CtType *parameters;
    size_t parameter_count;
    int complete, variadic;
} TypeInfo;

/* Type metadata is immutable once a row is published. Keep the overwhelmingly
 * common valid-handle path inline; the slow path seeds the table and handles a
 * bad handle. The registry was already process-global, so exposing its read
 * side here does not change its lifetime or concurrency semantics. */
#define CT_TYPE_LIMIT 4096

extern TypeInfo *ct_type_table;
extern size_t ct_type_table_count;
extern CtType ct_pointer_cache[CT_TYPE_LIMIT]; /* stored as handle + 1; zero means absent */
const TypeInfo *type_info_slow(CtType type);

static inline const TypeInfo *type_info(CtType type) {
    return type >= 0 && (size_t)type < ct_type_table_count
        ? &ct_type_table[type] : type_info_slow(type);
}

_Static_assert((int)CT_VOID == (int)TY_VOID && (int)CT_INT == (int)TY_INT, "basic handles must equal their kinds");

static inline TypeKind type_kind(CtType type) {
    return (unsigned)type <= TY_VOID ? (TypeKind)type : type_info(type)->kind;
}
static inline size_t type_size(CtType type) { return type_info(type)->size; }
static inline size_t ct_type_align(CtType type) { return type_info(type)->align; }
#ifndef CT_TYPES_IMPLEMENTATION
#define ct_type_size(type) type_size(type)
#endif

CtType type_pointer_slow(CtType target);
static inline CtType type_pointer(CtType target) {
    return target >= 0 && target < CT_TYPE_LIMIT && ct_pointer_cache[target]
        ? ct_pointer_cache[target] - 1 : type_pointer_slow(target);
}
CtType type_array(CtType element, size_t count);
CtType type_function(CtType result, const CtType *parameters, size_t count, int variadic);
CtType type_aggregate(int is_union, const char *tag, size_t tag_length);
int type_add_member(CtType aggregate, const char *name, size_t length, CtType type);
int type_finish(CtType aggregate);
const Member *type_member(CtType aggregate, const char *name, size_t length);

/* The element type of an array or the referenced type of a pointer. */
static inline CtType type_target(CtType type) { return type_info(type)->target; }
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
static inline CtType type_promote(CtType type) {
    const TypeInfo *info = type_info(type);
    if (info->kind > TY_ULLONG || info->rank >= type_info(CT_INT)->rank) return type;
    return CT_INT;
}
/* C17 6.3.1.8: the type the two operands of an arithmetic operator share. */
static inline CtType type_common(CtType left, CtType right) {
    TypeKind left_kind = type_kind(left), right_kind = type_kind(right);
    if (left_kind == TY_DOUBLE || right_kind == TY_DOUBLE) return CT_DOUBLE;
    if (left_kind == TY_FLOAT || right_kind == TY_FLOAT) return CT_FLOAT;
    left = type_promote(left);
    right = type_promote(right);
    if (left == right) return left;
    const TypeInfo *a = type_info(left), *b = type_info(right);
    if (a->is_signed == b->is_signed) return a->rank > b->rank ? left : right;
    CtType unsigned_type = a->is_signed ? right : left;
    CtType signed_type = a->is_signed ? left : right;
    if (type_info(unsigned_type)->rank >= type_info(signed_type)->rank) return unsigned_type;
    if (type_size(signed_type) > type_size(unsigned_type)) return signed_type;
    return (CtType)(signed_type + 1);
}
/* The widest value the type can hold, for range checks and wrapping. */
static inline uint64_t type_mask(CtType type) { return type_info(type)->mask; }
static inline int64_t type_maximum(CtType type) { return type_info(type)->maximum; }
static inline int64_t type_minimum(CtType type) { return type_info(type)->minimum; }
/* Objects that a CtValue carries directly rather than by address. */
static inline int type_is_scalar(CtType type) { return type_is_number(type) || type_is_pointer(type); }

void types_retain(void);
void types_release(void);

#endif
