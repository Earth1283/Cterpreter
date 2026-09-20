#include "types.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define TYPE_LIMIT 4096

static TypeInfo *table;
static size_t table_count, table_capacity;
static unsigned table_references;

#define SCALAR(k, type, sign, order) \
    {.kind = (k), .size = sizeof(type), .align = sizeof(type), .is_signed = (sign), .rank = (order), .complete = 1}

static const TypeInfo scalars[] = {
    SCALAR(TY_BOOL, _Bool, 0, 0),
    SCALAR(TY_CHAR, char, (char)-1 < 0, 1),
    SCALAR(TY_SCHAR, signed char, 1, 1),
    SCALAR(TY_UCHAR, unsigned char, 0, 1),
    SCALAR(TY_SHORT, short, 1, 2),
    SCALAR(TY_USHORT, unsigned short, 0, 2),
    SCALAR(TY_INT, int, 1, 3),
    SCALAR(TY_UINT, unsigned, 0, 3),
    SCALAR(TY_LONG, long, 1, 4),
    SCALAR(TY_ULONG, unsigned long, 0, 4),
    SCALAR(TY_LLONG, long long, 1, 5),
    SCALAR(TY_ULLONG, unsigned long long, 0, 5),
    SCALAR(TY_FLOAT, float, 1, 6),
    SCALAR(TY_DOUBLE, double, 1, 7),
    {.kind = TY_VOID, .size = 0, .align = 1, .complete = 1}
};

_Static_assert(sizeof(long long) <= sizeof(int64_t), "Cterpreter requires long long to fit in 64 bits");

static int seed(void) {
    if (table_count) return 1;
    table = calloc(sizeof scalars / sizeof scalars[0], sizeof *table);
    if (!table) return 0;
    table_capacity = sizeof scalars / sizeof scalars[0];
    memcpy(table, scalars, sizeof scalars);
    table_count = table_capacity;
    return 1;
}

static CtType append(TypeInfo info) {
    if (!seed() || table_count >= TYPE_LIMIT) return CT_VOID;
    if (table_count == table_capacity) {
        size_t capacity = table_capacity * 2;
        TypeInfo *grown = realloc(table, capacity * sizeof *grown);
        if (!grown) return CT_VOID;
        table = grown;
        table_capacity = capacity;
    }
    table[table_count] = info;
    return (CtType)table_count++;
}

const TypeInfo *type_info(CtType type) {
    if (!seed()) return &scalars[TY_VOID];
    if (type < 0 || (size_t)type >= table_count) return &scalars[TY_VOID];
    return &table[type];
}

TypeKind type_kind(CtType type) { return type_info(type)->kind; }
size_t ct_type_size(CtType type) { return type_info(type)->size; }
size_t ct_type_align(CtType type) { return type_info(type)->align; }
CtType type_target(CtType type) { return type_info(type)->target; }

CtType type_decay(CtType type) {
    const TypeInfo *info = type_info(type);
    if (info->kind == TY_ARRAY) return type_pointer(info->target);
    if (info->kind == TY_FUNCTION) return type_pointer(type);
    return type;
}

uint64_t type_mask(CtType type) {
    size_t bits = ct_type_size(type) * CHAR_BIT;
    return bits >= 64 ? UINT64_MAX : (UINT64_C(1) << bits) - 1;
}

int64_t type_maximum(CtType type) {
    const TypeInfo *info = type_info(type);
    uint64_t mask = type_mask(type);
    return info->is_signed ? (int64_t)(mask >> 1) : (int64_t)mask;
}

int64_t type_minimum(CtType type) {
    const TypeInfo *info = type_info(type);
    return info->is_signed ? -type_maximum(type) - 1 : 0;
}

CtType type_promote(CtType type) {
    const TypeInfo *info = type_info(type);
    if (!type_is_integer(type) || info->rank >= type_info(CT_INT)->rank) return type;
    /* Every narrower type fits in int here, so the promotion never goes unsigned. */
    return CT_INT;
}

CtType type_common(CtType left, CtType right) {
    if (type_kind(left) == TY_DOUBLE || type_kind(right) == TY_DOUBLE) return CT_DOUBLE;
    if (type_kind(left) == TY_FLOAT || type_kind(right) == TY_FLOAT) return CT_FLOAT;
    left = type_promote(left);
    right = type_promote(right);
    if (left == right) return left;
    const TypeInfo *a = type_info(left), *b = type_info(right);
    if (a->is_signed == b->is_signed) return a->rank > b->rank ? left : right;
    CtType unsigned_type = a->is_signed ? right : left;
    CtType signed_type = a->is_signed ? left : right;
    if (type_info(unsigned_type)->rank >= type_info(signed_type)->rank) return unsigned_type;
    if (ct_type_size(signed_type) > ct_type_size(unsigned_type)) return signed_type;
    return (CtType)(signed_type + 1);
}

CtType type_pointer(CtType target) {
    if (!seed()) return CT_VOID;
    for (size_t i = 0; i < table_count; ++i)
        if (table[i].kind == TY_POINTER && table[i].target == target) return (CtType)i;
    return append((TypeInfo){.kind = TY_POINTER, .target = target, .size = sizeof(uint64_t),
                             .align = sizeof(uint64_t), .complete = 1});
}

CtType type_array(CtType element, size_t count) {
    const TypeInfo *info = type_info(element);
    if (!info->complete || !info->size || count > TYPE_LIMIT * TYPE_LIMIT) return CT_VOID;
    if (count && info->size > (size_t)-1 / count) return CT_VOID;
    for (size_t i = 0; i < table_count; ++i)
        if (table[i].kind == TY_ARRAY && table[i].target == element && table[i].count == count) return (CtType)i;
    return append((TypeInfo){.kind = TY_ARRAY, .target = element, .count = count,
                             .size = info->size * count, .align = info->align, .complete = count != 0});
}

static int same_parameters(const TypeInfo *info, const CtType *parameters, size_t count, int variadic) {
    if (info->parameter_count != count || info->variadic != variadic) return 0;
    for (size_t i = 0; i < count; ++i)
        if (info->parameters[i] != parameters[i]) return 0;
    return 1;
}

CtType type_function(CtType result, const CtType *parameters, size_t count, int variadic) {
    if (!seed()) return CT_VOID;
    for (size_t i = 0; i < table_count; ++i)
        if (table[i].kind == TY_FUNCTION && table[i].target == result &&
            same_parameters(&table[i], parameters, count, variadic)) return (CtType)i;
    CtType *copied = count ? malloc(count * sizeof *copied) : NULL;
    if (count && !copied) return CT_VOID;
    if (count) memcpy(copied, parameters, count * sizeof *copied);
    CtType type = append((TypeInfo){.kind = TY_FUNCTION, .target = result, .parameters = copied,
                                    .parameter_count = count, .variadic = variadic, .align = 1});
    if (type == CT_VOID) free(copied);
    return type;
}

CtType type_aggregate(int is_union, const char *tag, size_t tag_length) {
    char *name = NULL;
    if (tag_length) {
        name = malloc(tag_length + 1);
        if (!name) return CT_VOID;
        memcpy(name, tag, tag_length);
        name[tag_length] = '\0';
    }
    CtType type = append((TypeInfo){.kind = is_union ? TY_UNION : TY_STRUCT, .tag = name, .align = 1});
    if (type == CT_VOID) free(name);
    return type;
}

int type_add_member(CtType aggregate, const char *name, size_t length, CtType type) {
    if (!seed() || aggregate <= CT_VOID || (size_t)aggregate >= table_count) return 0;
    TypeInfo *info = &table[aggregate];
    const TypeInfo *member = type_info(type);
    if (info->complete || !member->complete || !member->size) return 0;
    if (type_member(aggregate, name, length)) return 0;
    Member *members = realloc(info->members, (info->member_count + 1) * sizeof *members);
    if (!members) return 0;
    info->members = members;
    Member *slot = &members[info->member_count];
    slot->name = malloc(length + 1);
    if (!slot->name) return 0;
    memcpy(slot->name, name, length);
    slot->name[length] = '\0';
    slot->length = length;
    slot->type = type;
    if (info->kind == TY_UNION) {
        slot->offset = 0;
        if (member->size > info->size) info->size = member->size;
    } else {
        size_t padding = info->size % member->align;
        slot->offset = info->size + (padding ? member->align - padding : 0);
        info->size = slot->offset + member->size;
    }
    if (member->align > info->align) info->align = member->align;
    ++info->member_count;
    return 1;
}

int type_finish(CtType aggregate) {
    if (!seed() || aggregate <= CT_VOID || (size_t)aggregate >= table_count) return 0;
    TypeInfo *info = &table[aggregate];
    if (info->complete || !info->member_count) return 0;
    size_t padding = info->size % info->align;
    if (padding) info->size += info->align - padding;
    info->complete = 1;
    return 1;
}

const Member *type_member(CtType aggregate, const char *name, size_t length) {
    const TypeInfo *info = type_info(aggregate);
    for (size_t i = 0; i < info->member_count; ++i)
        if (info->members[i].length == length && !memcmp(info->members[i].name, name, length))
            return &info->members[i];
    return NULL;
}

typedef struct { char *data; size_t length, capacity; } Text;

static void text_add(Text *text, const char *source, size_t length) {
    if (text->length + length + 1 > text->capacity) {
        size_t capacity = text->length + length + 64;
        char *grown = realloc(text->data, capacity);
        if (!grown) return;
        text->data = grown;
        text->capacity = capacity;
    }
    if (!text->data) return;
    memcpy(text->data + text->length, source, length);
    text->length += length;
    text->data[text->length] = '\0';
}

static void text_put(Text *text, const char *source) { text_add(text, source, strlen(source)); }

static void text_insert(Text *text, const char *source) {
    size_t length = strlen(source);
    text_add(text, source, length);
    if (!text->data || text->length < length) return;
    memmove(text->data + length, text->data, text->length - length);
    memcpy(text->data, source, length);
}

static void text_number(Text *text, size_t value) {
    char digits[24];
    size_t length = 0;
    do { digits[length++] = (char)('0' + value % 10); value /= 10; } while (value);
    char reversed[24];
    for (size_t i = 0; i < length; ++i) reversed[i] = digits[length - 1 - i];
    text_add(text, reversed, length);
}

static const char *basic_name(TypeKind kind) {
    static const char *names[] = {
        "_Bool", "char", "signed char", "unsigned char", "short", "unsigned short",
        "int", "unsigned int", "long", "unsigned long", "long long", "unsigned long long",
        "float", "double", "void"
    };
    return kind <= TY_VOID ? names[kind] : "int";
}

/* C declarator order: the prefix precedes the (empty) declarator, the suffix follows it. */
static void name_parts(CtType type, Text *prefix, Text *suffix, unsigned depth) {
    const TypeInfo *info = type_info(type);
    if (depth > 32) { text_put(prefix, "..."); return; }
    switch (info->kind) {
        case TY_POINTER: {
            name_parts(info->target, prefix, suffix, depth + 1);
            TypeKind target = type_kind(info->target);
            if (target == TY_ARRAY || target == TY_FUNCTION) {
                text_put(prefix, " (*");
                text_insert(suffix, ")");
            } else text_put(prefix, prefix->length && prefix->data[prefix->length - 1] == '*' ? "*" : " *");
            break;
        }
        case TY_ARRAY: {
            Text inner = {0};
            name_parts(info->target, prefix, &inner, depth + 1);
            Text bounds = {0};
            text_put(&bounds, "[");
            text_number(&bounds, info->count);
            text_put(&bounds, "]");
            text_put(&bounds, inner.data ? inner.data : "");
            text_insert(suffix, bounds.data ? bounds.data : "");
            free(inner.data);
            free(bounds.data);
            break;
        }
        case TY_FUNCTION: {
            Text inner = {0};
            name_parts(info->target, prefix, &inner, depth + 1);
            Text parameters = {0};
            text_put(&parameters, "(");
            for (size_t i = 0; i < info->parameter_count; ++i) {
                if (i) text_put(&parameters, ", ");
                char name[128];
                ct_type_name(info->parameters[i], name, sizeof name);
                text_put(&parameters, name);
            }
            if (info->variadic) text_put(&parameters, info->parameter_count ? ", ..." : "...");
            else if (!info->parameter_count) text_put(&parameters, "void");
            text_put(&parameters, ")");
            text_put(&parameters, inner.data ? inner.data : "");
            text_insert(suffix, parameters.data ? parameters.data : "");
            free(inner.data);
            free(parameters.data);
            break;
        }
        case TY_STRUCT: case TY_UNION:
            text_put(prefix, info->kind == TY_UNION ? "union " : "struct ");
            text_put(prefix, info->tag ? info->tag : "<anonymous>");
            break;
        case TY_VOID: text_put(prefix, "void"); break;
        default: text_put(prefix, basic_name(info->kind)); break;
    }
}

void ct_type_name(CtType type, char *buffer, size_t capacity) {
    if (!capacity) return;
    Text prefix = {0}, suffix = {0};
    name_parts(type, &prefix, &suffix, 0);
    size_t used = 0;
    const char *parts[] = {prefix.data, suffix.data};
    for (size_t i = 0; i < 2; ++i)
        for (const char *cursor = parts[i] ? parts[i] : ""; *cursor && used + 1 < capacity; ++cursor)
            buffer[used++] = *cursor;
    buffer[used] = '\0';
    free(prefix.data);
    free(suffix.data);
}

void types_retain(void) { ++table_references; }

void types_release(void) {
    if (!table_references || --table_references) return;
    for (size_t i = 0; i < table_count; ++i) {
        for (size_t j = 0; j < table[i].member_count; ++j) free(table[i].members[j].name);
        free(table[i].members);
        free(table[i].parameters);
        free(table[i].tag);
    }
    free(table);
    table = NULL;
    table_count = table_capacity = 0;
}
