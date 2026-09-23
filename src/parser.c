#include "parser.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG_CONSTANT 4
#define SUFFIX_LIMIT 8
#define TYPE_LIMIT 4096
#define NODES_PER_BLOCK 128

struct NodeBlock {
    NodeBlock *next;
    size_t used;
    Node nodes[NODES_PER_BLOCK];
};

typedef struct Alias Alias;
struct Alias { Node *node; unsigned scope; Alias *next; };

typedef struct {
    Lexer lexer;
    Token token;
    Unit *unit;
    const Unit *previous;
    CtError *error;
    CtStatus status;
    size_t nodes;
    unsigned depth, loops, functions, switches, scope;
    Alias *aliases;
    Alias *objects; /* kept apart so that type-name lookups never walk them */
    CtType function_type;
} Parser;

typedef struct { Lexer lexer; Token token; } Position;

static void fail_at(Parser *parser, Token token, const char *message) {
    if (parser->status != CT_OK) return;
    parser->status = token.kind == TK_EOF ? CT_INCOMPLETE : CT_ERROR;
    *parser->error = (CtError){.line = token.line, .column = token.column};
    (void)snprintf(parser->error->message, sizeof parser->error->message, "%s", message);
}

static void fail(Parser *parser, const char *message) { fail_at(parser, parser->token, message); }

static void next(Parser *parser) {
    if (parser->status != CT_OK) return;
    parser->token = lexer_next(&parser->lexer);
    if (parser->token.kind == TK_ERROR) {
        parser->status = parser->lexer.incomplete ? CT_INCOMPLETE : CT_ERROR;
        *parser->error = parser->lexer.error;
    }
}

static int accept(Parser *parser, int kind) {
    if (parser->status != CT_OK || parser->token.kind != kind) return 0;
    next(parser);
    return 1;
}

static void expect(Parser *parser, int kind, const char *message) {
    if (!accept(parser, kind)) fail(parser, message);
}

static Position mark(const Parser *parser) { return (Position){parser->lexer, parser->token}; }
static void restore(Parser *parser, Position position) { parser->lexer = position.lexer; parser->token = position.token; }

static Node *node(Parser *parser, NodeKind kind, Token token) {
    if (parser->status != CT_OK) return NULL;
    if (++parser->nodes > 16384) {
        fail(parser, "syntax tree size limit exceeded");
        parser->status = CT_ERROR;
        return NULL;
    }
    NodeBlock *block = parser->unit->node_blocks;
    if (!block || block->used == NODES_PER_BLOCK) {
        block = calloc(1, sizeof *block);
        if (!block) {
            fail(parser, "out of memory");
            parser->status = CT_ERROR;
            return NULL;
        }
        block->next = parser->unit->node_blocks;
        parser->unit->node_blocks = block;
    }
    Node *result = &block->nodes[block->used++];
    result->kind = kind;
    result->token = token;
    result->allocated_next = parser->unit->allocations;
    parser->unit->allocations = result;
    return result;
}

static int precedence(int kind) {
    if (is_assignment(kind)) return 1;
    switch (kind) {
        case '?': return 2;
        case TK_OR: return 3;
        case TK_AND: return 4;
        case '|': return 5;
        case '^': return 6;
        case '&': return 7;
        case TK_EQ: case TK_NE: return 8;
        case '<': case '>': case TK_LE: case TK_GE: return 9;
        case TK_SHL: case TK_SHR: return 10;
        case '+': case '-': return 11;
        case '*': case '/': case '%': return 12;
        default: return 0;
    }
}

static Node *expression(Parser *parser, int minimum);
static Node *comma_expression(Parser *parser);
static Node *statement(Parser *parser, int top_level);
static Node *initializer(Parser *parser);
static Node *static_assertion(Parser *parser);
static CtType type_specifier(Parser *parser, int *storage, int *is_const, Node ***tail);
static CtType declarator(Parser *parser, CtType base, Token *name, Node **parameters, int *variadic);
static int is_qualifier(int kind);

static int enter(Parser *parser) {
    if (++parser->depth <= 128) return 1;
    fail(parser, "syntax nesting limit exceeded");
    parser->status = CT_ERROR;
    --parser->depth;
    return 0;
}

static int same_name(Token token, Token other) {
    return token.length && token.length == other.length && !memcmp(token.start, other.start, token.length);
}

static Node *find_alias(Parser *parser, Token token, int tag_kind, int current_scope_only) {
    if (token.kind != TK_NAME) return NULL;
    for (Alias *alias = parser->aliases; alias; alias = alias->next) {
        if (current_scope_only && alias->scope != parser->scope) break;
        if (alias->node->tag_kind == tag_kind && same_name(alias->node->token, token)) return alias->node;
    }
    if (current_scope_only && parser->scope) return NULL;
    for (const Unit *unit = parser->previous; unit; unit = unit->next)
        for (Node *entry = unit->aliases; entry; entry = entry->alias_next)
            if (entry->tag_kind == tag_kind && same_name(entry->token, token)) return entry;
    return NULL;
}

static void declare_alias(Parser *parser, Node *entry) {
    Alias *alias = calloc(1, sizeof *alias);
    if (!alias) { fail(parser, "out of memory"); parser->status = CT_ERROR; return; }
    entry->local = parser->scope != 0;
    if (!entry->local) {
        entry->alias_next = parser->unit->aliases;
        parser->unit->aliases = entry;
    }
    alias->node = entry;
    alias->scope = parser->scope;
    alias->next = parser->aliases;
    parser->aliases = alias;
    parser->unit->has_functions = 1;
}

/* Objects are remembered only while their scope is open, for sizeof in constant expressions. */
static void declare_object(Parser *parser, Node *declaration) {
    if (!declaration->token.length) return;
    Alias *object = calloc(1, sizeof *object);
    if (!object) { fail(parser, "out of memory"); parser->status = CT_ERROR; return; }
    object->node = declaration;
    object->scope = parser->scope;
    object->next = parser->objects;
    parser->objects = object;
}

static Node *find_object(Parser *parser, Token token) {
    for (Alias *object = parser->objects; object; object = object->next)
        if (same_name(object->node->token, token)) return object->node;
    return NULL;
}

static void pop_scoped(Alias **list, unsigned scope) {
    while (*list && (*list)->scope > scope) {
        Alias *dead = *list;
        *list = dead->next;
        free(dead);
    }
}

static void free_aliases(Alias *list) {
    while (list) {
        Alias *next = list->next;
        free(list);
        list = next;
    }
}

static void pop_aliases(Parser *parser) {
    pop_scoped(&parser->aliases, parser->scope);
    pop_scoped(&parser->objects, parser->scope);
}

static Node *declare_tag(Parser *parser, Token tag, CtType type, int tag_kind) {
    Node *entry = node(parser, N_TYPEDEF, tag);
    if (!entry) return NULL;
    entry->type = type;
    entry->tag_kind = tag_kind;
    declare_alias(parser, entry);
    return entry;
}

static int begins_type(Parser *parser, Token token) {
    switch (token.kind) {
        case TK_INT: case TK_DOUBLE: case TK_CHAR: case TK_VOID: case TK_CONST: case TK_STATIC:
        case TK_TYPEDEF: case TK_ENUM: case TK_STRUCT: case TK_UNION: case TK_SIGNED:
        case TK_UNSIGNED: case TK_SHORT: case TK_LONG: case TK_FLOAT: case TK_BOOL:
        case TK_VOLATILE: case TK_RESTRICT: case TK_EXTERN: case TK_REGISTER:
        case TK_INLINE: case TK_AUTO: return 1;
        case TK_NAME: return find_alias(parser, token, TAG_NAME, 0) != NULL;
        default: return 0;
    }
}

/* The type sizeof sees, for the operand forms whose type is known while parsing. */
static int operand_type(Parser *parser, Node *operand, CtType *type) {
    CtType inner;
    switch (operand->kind) {
        case N_NAME: {
            Node *object = find_object(parser, operand->token);
            if (!object) return 0;
            *type = object->type;
            return 1;
        }
        case N_STRING: *type = type_array(CT_CHAR, operand->text_length + 1); return 1;
        case N_VALUE: *type = operand->token.value.type; return 1;
        case N_CAST: case N_COMPOUND: *type = operand->type; return 1;
        case N_SIZEOF: case N_ALIGNOF: *type = CT_ULONG; return 1;
        case N_INDEX:
            if (!operand_type(parser, operand->left, &inner) || !type_is_pointer(inner = type_decay(inner))) return 0;
            *type = type_target(inner);
            return 1;
        case N_UNARY:
            if (!operand_type(parser, operand->left, &inner)) return 0;
            if (operand->token.kind == '&') { *type = type_pointer(inner); return 1; }
            if (operand->token.kind != '*' || !type_is_pointer(inner = type_decay(inner))) return 0;
            *type = type_target(inner);
            return 1;
        case N_MEMBER: {
            if (!operand_type(parser, operand->left, &inner)) return 0;
            if (operand->through_pointer) {
                if (!type_is_pointer(inner = type_decay(inner))) return 0;
                inner = type_target(inner);
            }
            const Member *member = type_member(inner, operand->token.start, operand->token.length);
            if (!member) return 0;
            *type = member->type;
            return 1;
        }
        default: return 0;
    }
}

/* Folds the constant expressions the language needs before execution: array
 * bounds, enumerator values and array designators. Works in long long. */
static int constant_value(Parser *parser, Node *source, int64_t *result) {
    if (!source) return 0;
    int64_t left = 0, right = 0;
    switch (source->kind) {
        case N_VALUE:
            if (!type_is_integer(source->token.value.type)) return 0;
            *result = source->token.value.as.integer;
            return 1;
        case N_NAME: {
            Node *alias = find_alias(parser, source->token, TAG_CONSTANT, 0);
            if (!alias || !alias->left || alias->left->kind != N_VALUE) return 0;
            *result = alias->left->token.value.as.integer;
            return 1;
        }
        case N_SIZEOF: case N_ALIGNOF: {
            CtType type = source->type;
            if (source->left && !operand_type(parser, source->left, &type)) return 0;
            if (!type_info(type)->complete) return 0;
            size_t size = source->kind == N_SIZEOF ? ct_type_size(type) : ct_type_align(type);
            if (!size || size > INT64_MAX) return 0;
            *result = (int64_t)size;
            return 1;
        }
        case N_CAST:
            if (!type_is_integer(source->type)) return 0;
            if (!constant_value(parser, source->left, result)) return 0;
            *result = type_is_signed(source->type)
                ? (int64_t)((uint64_t)*result & type_mask(source->type))
                : (int64_t)((uint64_t)*result & type_mask(source->type));
            if (type_is_signed(source->type) && *result > type_maximum(source->type))
                *result -= (int64_t)type_mask(source->type) + 1;
            return 1;
        case N_UNARY:
            if (!constant_value(parser, source->left, &left)) return 0;
            switch (source->token.kind) {
                case '+': *result = left; return 1;
                case '-': if (left == INT64_MIN) return 0; *result = -left; return 1;
                case '~': *result = ~left; return 1;
                case '!': *result = !left; return 1;
                default: return 0;
            }
        case N_CONDITIONAL: {
            int64_t condition = 0;
            if (!constant_value(parser, source->left, &condition)) return 0;
            return constant_value(parser, condition ? source->right : source->third, result);
        }
        case N_BINARY: {
            int kind = source->token.kind;
            if (is_assignment(kind)) return 0;
            if (!constant_value(parser, source->left, &left)) return 0;
            if (kind == TK_AND && !left) { *result = 0; return 1; }
            if (kind == TK_OR && left) { *result = 1; return 1; }
            if (!constant_value(parser, source->right, &right)) return 0;
            /* Anything that could overflow a long long is simply not folded. */
            if (left > INT32_MAX || left < INT32_MIN || right > INT32_MAX || right < INT32_MIN) return 0;
            switch (kind) {
                case '+': *result = left + right; return 1;
                case '-': *result = left - right; return 1;
                case '*': *result = left * right; return 1;
                case '/': case '%':
                    if (!right) return 0;
                    *result = kind == '/' ? left / right : left % right;
                    return 1;
                case '&': *result = left & right; return 1;
                case '|': *result = left | right; return 1;
                case '^': *result = left ^ right; return 1;
                case TK_SHL: case TK_SHR:
                    if (right < 0 || right >= 32 || (kind == TK_SHL && left < 0)) return 0;
                    *result = kind == TK_SHL ? left << right : left >> right;
                    return 1;
                case TK_EQ: *result = left == right; return 1;
                case TK_NE: *result = left != right; return 1;
                case '<': *result = left < right; return 1;
                case '>': *result = left > right; return 1;
                case TK_LE: *result = left <= right; return 1;
                case TK_GE: *result = left >= right; return 1;
                case TK_AND: *result = left && right; return 1;
                case TK_OR: *result = left || right; return 1;
                default: return 0;
            }
        }
        default: return 0;
    }
}

static void skip_parenthesized(Parser *parser) {
    unsigned nesting = 0;
    do {
        if (parser->token.kind == TK_EOF) { fail(parser, "unterminated declarator"); return; }
        if (parser->token.kind == '(') ++nesting;
        else if (parser->token.kind == ')') --nesting;
        next(parser);
    } while (parser->status == CT_OK && nesting);
}

typedef struct {
    int function, sized, variadic;
    size_t count, parameter_count, parameter_capacity;
    CtType *parameters;
    Node *nodes;
} Suffix;

static void parameter_list(Parser *parser, Suffix *suffix) {
    suffix->function = 1;
    if (parser->token.kind == TK_VOID) {
        Position look = mark(parser);
        next(parser);
        if (accept(parser, ')')) return;
        restore(parser, look);
    }

    if (accept(parser, ')')) return;
    Node **tail = &suffix->nodes;
    for (;;) {
        if (accept(parser, TK_ELLIPSIS)) {
            if (!suffix->parameter_count) { fail(parser, "'...' requires a preceding parameter"); break; }
            suffix->variadic = 1;
            break;
        }
        int storage = 0, is_const = 0;
        CtType base = type_specifier(parser, &storage, &is_const, NULL);
        Token name;
        CtType type = type_decay(declarator(parser, base, &name, NULL, NULL));
        if (parser->status != CT_OK) return;
        if (!type_info(type)->complete || !ct_type_size(type)) { fail(parser, "parameter has an incomplete type"); break; }
        if (suffix->parameter_count == suffix->parameter_capacity) {
            size_t capacity = suffix->parameter_capacity ? suffix->parameter_capacity * 2 : 4;
            CtType *grown = realloc(suffix->parameters, capacity * sizeof *grown);
            if (!grown) { fail(parser, "out of memory"); parser->status = CT_ERROR; break; }
            suffix->parameters = grown;
            suffix->parameter_capacity = capacity;
        }
        suffix->parameters[suffix->parameter_count++] = type;
        Node *parameter = node(parser, N_DECLARATION, name);
        if (!parameter) break;
        parameter->type = type;
        *tail = parameter;
        tail = &parameter->next;
        if (!accept(parser, ',')) break;
    }
    expect(parser, ')', "expected ')' after parameters");
}

static CtType declarator(Parser *parser, CtType base, Token *name, Node **parameters, int *variadic) {
    if (parser->status != CT_OK || !enter(parser)) return base;
    while (accept(parser, '*')) {
        base = type_pointer(base);
        while (parser->status == CT_OK && is_qualifier(parser->token.kind)) next(parser);
    }
    if (name) { *name = parser->token; name->length = 0; }
    Position before = mark(parser);
    int nested = 0;
    if (parser->token.kind == '(') {
        Lexer lookahead = parser->lexer;
        Token after = lexer_next(&lookahead);
        nested = after.kind == '*' || (after.kind == TK_NAME && !begins_type(parser, after));
    }
    if (nested) skip_parenthesized(parser);
    else if (parser->token.kind == TK_NAME) {
        if (name) *name = parser->token;
        next(parser);
    }
    Suffix suffixes[SUFFIX_LIMIT] = {{0}};
    size_t count = 0;
    while (parser->status == CT_OK && (parser->token.kind == '[' || parser->token.kind == '(')) {
        if (count == SUFFIX_LIMIT) { fail(parser, "declarator complexity limit exceeded"); break; }
        Suffix *suffix = &suffixes[count++];
        if (accept(parser, '[')) {
            if (parser->token.kind != ']') {
                Node *size = expression(parser, 1);
                int64_t value = 0;
                if (parser->status != CT_OK) break;
                if (!constant_value(parser, size, &value) || value <= 0 || value > (int64_t)TYPE_LIMIT * TYPE_LIMIT) {
                    fail(parser, "array size must be a positive integer constant expression");
                    break;
                }
                suffix->count = (size_t)value;
                suffix->sized = 1;
            }
            expect(parser, ']', "expected ']' after array size");
        } else {
            next(parser);
            parameter_list(parser, suffix);
        }
    }
    for (size_t i = count; i-- > 0 && parser->status == CT_OK;) {
        Suffix *suffix = &suffixes[i];
        if (suffix->function) {
            if (type_is_function(base) || type_is_array(base)) fail(parser, "a function cannot return an array or a function");
            else base = type_function(base, suffix->parameters, suffix->parameter_count, suffix->variadic);
            if (parameters) *parameters = suffix->nodes;
            if (variadic) *variadic = suffix->variadic;
        } else if (!suffix->sized && i) fail(parser, "only the first array dimension may be unsized");
        else if (type_is_function(base)) fail(parser, "an array cannot hold functions");
        else {
            CtType array = type_array(base, suffix->count);
            if (array == CT_VOID) fail(parser, "an array requires a complete element type");
            else base = array;
        }
    }
    for (size_t i = 0; i < count; ++i) free(suffixes[i].parameters);
    if (nested && parser->status == CT_OK) {
        Position after_suffixes = mark(parser);
        restore(parser, before);
        expect(parser, '(', "expected '(' in declarator");
        base = declarator(parser, base, name, parameters, variadic);
        expect(parser, ')', "expected ')' in declarator");
        if (parser->status == CT_OK) restore(parser, after_suffixes);
    }
    --parser->depth;
    return base;
}

static CtType type_name_of(Parser *parser) {
    int storage = 0, is_const = 0;
    CtType base = type_specifier(parser, &storage, &is_const, NULL);
    Token name;
    CtType type = declarator(parser, base, &name, NULL, NULL);
    if (name.length) fail(parser, "a type name cannot declare a variable");
    return type;
}

static void aggregate_members(Parser *parser, CtType aggregate) {
    while (parser->status == CT_OK && parser->token.kind != '}' && parser->token.kind != TK_EOF) {
        if (parser->token.kind == TK_STATIC_ASSERT) { (void)static_assertion(parser); continue; }
        int storage = 0, is_const = 0;
        CtType base = type_specifier(parser, &storage, &is_const, NULL);
        if (storage == TK_STATIC) { fail(parser, "a member cannot be static"); break; }
        do {
            Token name;
            CtType type = declarator(parser, base, &name, NULL, NULL);
            if (parser->status != CT_OK) return;
            if (!name.length) { fail(parser, "expected a member name"); return; }
            if (!type_add_member(aggregate, name.start, name.length, type))
                { fail(parser, "member has a duplicate name or an incomplete type"); return; }
        } while (accept(parser, ','));
        expect(parser, ';', "expected ';' after the member declaration");
    }
    expect(parser, '}', "expected '}' after members");
    if (parser->status == CT_OK && !type_finish(aggregate)) fail(parser, "an aggregate requires at least one member");
}

static CtType aggregate_specifier(Parser *parser) {
    int is_union = parser->token.kind == TK_UNION;
    int tag_kind = is_union ? TAG_UNION : TAG_STRUCT;
    next(parser);
    Token tag = parser->token;
    int named = tag.kind == TK_NAME;
    if (named) next(parser);
    if (parser->token.kind != '{') {
        if (!named) { fail(parser, "expected a tag or '{'"); return CT_INT; }
        Node *existing = find_alias(parser, tag, tag_kind, 0);
        if (existing) return existing->type;
        CtType type = type_aggregate(is_union, tag.start, tag.length);
        if (type == CT_VOID) { fail(parser, "type limit exceeded"); return CT_INT; }
        Node *entry = declare_tag(parser, tag, type, tag_kind);
        return entry ? type : CT_INT;
    }
    Node *existing = named ? find_alias(parser, tag, tag_kind, 1) : NULL;
    if (existing && type_info(existing->type)->complete) { fail(parser, "the tag is already defined"); return CT_INT; }
    CtType type = existing ? existing->type : type_aggregate(is_union, tag.start, tag.length);
    if (type == CT_VOID) { fail(parser, "type limit exceeded"); return CT_INT; }
    if (named && !existing && !declare_tag(parser, tag, type, tag_kind)) return CT_INT;
    next(parser);
    aggregate_members(parser, type);
    return type;
}

static CtType enum_specifier(Parser *parser, Node ***tail) {
    next(parser);
    Token tag = parser->token;
    int named = tag.kind == TK_NAME;
    if (named) next(parser);
    if (parser->token.kind != '{') {
        if (!named) { fail(parser, "expected a tag or '{'"); return CT_INT; }
        Node *existing = find_alias(parser, tag, TAG_ENUM, 0);
        if (!existing) fail(parser, "unknown enum tag");
        return CT_INT;
    }
    if (!tail) { fail(parser, "an enum definition is not allowed here"); return CT_INT; }
    if (named) {
        if (find_alias(parser, tag, TAG_ENUM, 1)) { fail(parser, "the tag is already defined"); return CT_INT; }
        if (!declare_tag(parser, tag, CT_INT, TAG_ENUM)) return CT_INT;
    }
    next(parser);
    int64_t value = 0;
    while (parser->status == CT_OK && parser->token.kind != '}') {
        Token name = parser->token;
        expect(parser, TK_NAME, "expected an enumerator name");
        if (find_alias(parser, name, TAG_CONSTANT, 1)) { fail(parser, "the enumerator is already defined"); break; }
        if (accept(parser, '=') && !constant_value(parser, expression(parser, 1), &value)) {
            if (parser->status == CT_OK) fail(parser, "an enumerator requires an integer constant expression");
            break;
        }
        if (value < INT_MIN || value > INT_MAX) { fail(parser, "enumerator value does not fit in an int"); break; }
        Node *enumerator = node(parser, N_ENUMERATOR, name);
        Node *literal = node(parser, N_VALUE, name);
        if (!enumerator || !literal) break;
        literal->token.kind = TK_INTEGER;
        literal->token.value = (CtValue){.type = CT_INT, .as.integer = value};
        enumerator->type = CT_INT;
        enumerator->is_const = 1;
        enumerator->tag_kind = TAG_CONSTANT;
        enumerator->left = literal;
        declare_alias(parser, enumerator);
        **tail = enumerator;
        *tail = &enumerator->next;
        if (value == INT_MAX && parser->token.kind == ',') { fail(parser, "enumerator value overflows int"); break; }
        ++value;
        if (!accept(parser, ',')) break;
    }
    expect(parser, '}', "expected '}' after enumerators");
    return CT_INT;
}

static int is_qualifier(int kind) { return kind == TK_CONST || kind == TK_VOLATILE || kind == TK_RESTRICT; }

static int is_storage(int kind) {
    return kind == TK_STATIC || kind == TK_EXTERN || kind == TK_REGISTER || kind == TK_INLINE || kind == TK_AUTO;
}

/* C17 6.7.2: the basic specifiers may appear in any order, so count them. */
typedef struct { int sign, shorts, longs, chars, ints, floats, doubles, bools, voids; } Specifiers;

static CtType basic_type(Parser *parser, const Specifiers *seen) {
    int width = seen->shorts + seen->chars + seen->floats + seen->doubles + seen->bools + seen->voids;
    if (seen->shorts > 1 || seen->longs > 2 || seen->chars > 1 || seen->ints > 1 ||
        seen->floats > 1 || seen->doubles > 1 || seen->bools > 1 || seen->voids > 1 ||
        (width > 1) || (seen->voids && (seen->ints || seen->longs || seen->sign)) ||
        (seen->bools && (seen->ints || seen->longs || seen->sign)) ||
        (seen->floats && (seen->ints || seen->longs || seen->sign)) ||
        (seen->doubles && (seen->ints || seen->sign)) ||
        (seen->chars && (seen->ints || seen->longs)) ||
        (seen->shorts && seen->longs)) {
        fail(parser, "conflicting type specifiers");
        return CT_INT;
    }
    if (seen->voids) return CT_VOID;
    if (seen->bools) return CT_BOOL;
    if (seen->doubles) {
        if (seen->longs) { fail(parser, "long double is not supported"); return CT_DOUBLE; }
        return CT_DOUBLE;
    }
    if (seen->floats) return CT_FLOAT;
    if (seen->chars) return seen->sign > 0 ? CT_SCHAR : seen->sign < 0 ? CT_UCHAR : CT_CHAR;
    CtType type = seen->shorts ? CT_SHORT : seen->longs >= 2 ? CT_LLONG : seen->longs ? CT_LONG : CT_INT;
    return seen->sign < 0 ? (CtType)(type + 1) : type;
}

static CtType type_specifier(Parser *parser, int *storage, int *is_const, Node ***tail) {
    Specifiers seen = {0};
    CtType named = -1;
    int count = 0;
    while (parser->status == CT_OK) {
        int kind = parser->token.kind;
        if (is_storage(kind)) {
            if (kind == TK_STATIC || kind == TK_EXTERN) *storage = kind;
            next(parser);
            continue;
        }
        if (is_qualifier(kind)) {
            if (kind == TK_CONST) *is_const = 1;
            next(parser);
            continue;
        }
        if (kind == TK_STRUCT || kind == TK_UNION || kind == TK_ENUM) {
            if (named >= 0 || count) { fail(parser, "conflicting type specifiers"); break; }
            named = kind == TK_ENUM ? enum_specifier(parser, tail) : aggregate_specifier(parser);
            ++count;
            continue;
        }
        if (kind == TK_NAME) {
            if (named >= 0 || count) break;
            Node *alias = find_alias(parser, parser->token, TAG_NAME, 0);
            if (!alias) break;
            named = alias->type;
            ++count;
            next(parser);
            continue;
        }
        switch (kind) {
            case TK_SIGNED: seen.sign = 1; break;
            case TK_UNSIGNED: seen.sign = -1; break;
            case TK_SHORT: ++seen.shorts; break;
            case TK_LONG: ++seen.longs; break;
            case TK_CHAR: ++seen.chars; break;
            case TK_INT: ++seen.ints; break;
            case TK_FLOAT: ++seen.floats; break;
            case TK_DOUBLE: ++seen.doubles; break;
            case TK_BOOL: ++seen.bools; break;
            case TK_VOID: ++seen.voids; break;
            default: kind = 0; break;
        }
        if (!kind) break;
        if (named >= 0) { fail(parser, "conflicting type specifiers"); break; }
        ++count;
        next(parser);
    }
    if (parser->status != CT_OK) return CT_INT;
    if (!count) { fail(parser, "expected a type"); return CT_INT; }
    return named >= 0 ? named : basic_type(parser, &seen);
}

static Node *primary(Parser *parser) {
    Token token = parser->token;
    static const char *va_names[] = {"__ct_va_start", "__ct_va_arg", "__ct_va_end", "__ct_va_copy"};
    for (size_t i = 0; token.kind == TK_NAME && i < sizeof va_names / sizeof va_names[0]; ++i) {
        if (token.length != strlen(va_names[i]) || memcmp(token.start, va_names[i], token.length)) continue;
        Node *result = node(parser, (NodeKind)(N_VA_START + i), token);
        if (!result) return NULL;
        next(parser);
        expect(parser, '(', "expected '(' after stdarg intrinsic");
        result->left = expression(parser, 1);
        if (result->kind != N_VA_END) {
            expect(parser, ',', "expected ',' in stdarg intrinsic");
            if (result->kind == N_VA_ARG) {
                result->type = type_name_of(parser);
                if (!type_info(result->type)->complete || !ct_type_size(result->type) || type_is_array(result->type))
                    fail(parser, "va_arg requires a complete non-array object type");
            } else result->right = expression(parser, 1);
        }
        expect(parser, ')', "expected ')' after stdarg intrinsic");
        return result;
    }
    if (accept(parser, TK_GENERIC)) {
        Node *result = node(parser, N_GENERIC, token);
        if (!result) return NULL;
        expect(parser, '(', "expected '(' after _Generic");
        result->left = expression(parser, 1);
        expect(parser, ',', "expected ',' after the controlling expression");
        Node **tail = &result->right;
        do {
            Node *association = node(parser, N_ASSOCIATION, parser->token);
            if (!association) break;
            *tail = association;
            tail = &association->next;
            if (accept(parser, TK_DEFAULT)) association->is_const = 1;
            else association->type = type_name_of(parser);
            expect(parser, ':', "expected ':' in a generic association");
            association->left = expression(parser, 1);
        } while (accept(parser, ','));
        expect(parser, ')', "expected ')' after the generic associations");
        return result;
    }
    if (accept(parser, '(')) {
        if (begins_type(parser, parser->token)) {
            CtType type = type_name_of(parser);
            expect(parser, ')', "expected ')' after the type");
            if (parser->token.kind == '{') {
                Node *result = node(parser, N_COMPOUND, token);
                if (result) { result->type = type; result->left = initializer(parser); }
                return result;
            }
            Node *result = node(parser, N_CAST, token);
            if (result) { result->type = type; result->left = expression(parser, 13); }
            return result;
        }
        Node *result = comma_expression(parser);
        expect(parser, ')', "expected ')'");
        return result;
    }
    if (token.kind == TK_SIZEOF || token.kind == TK_ALIGNOF) {
        next(parser);
        Node *result = node(parser, token.kind == TK_SIZEOF ? N_SIZEOF : N_ALIGNOF, token);
        if (!result) return NULL;
        if (parser->token.kind == '(') {
            Position open = mark(parser);
            next(parser);
            if (begins_type(parser, parser->token)) {
                result->type = type_name_of(parser);
                expect(parser, ')', "expected ')' after the sizeof operand");
                return result;
            }
            restore(parser, open);
        }
        if (token.kind == TK_ALIGNOF) fail(parser, "_Alignof requires a parenthesized type");
        result->left = expression(parser, 13);
        return result;
    }
    if (token.kind == TK_STRING) {
        Node *result = node(parser, N_STRING, token);
        if (!result) return NULL;
        do {
            size_t length = 0;
            char *decoded = lexer_decode(parser->token, &length, parser->error);
            if (!decoded) { parser->status = CT_ERROR; break; }
            char *joined = realloc(result->text, result->text_length + length + 1);
            if (!joined) { free(decoded); fail(parser, "out of memory"); break; }
            result->text = joined;
            memcpy(joined + result->text_length, decoded, length + 1);
            result->text_length += length;
            free(decoded);
            next(parser);
        } while (parser->status == CT_OK && parser->token.kind == TK_STRING);
        return result;
    }
    if (token.kind == '+' || token.kind == '-' || token.kind == '!' || token.kind == '~' ||
        token.kind == '&' || token.kind == '*' ||
        token.kind == TK_INCREMENT || token.kind == TK_DECREMENT) {
        next(parser);
        Node *result = node(parser, N_UNARY, token);
        if (result) result->left = expression(parser, 13);
        return result;
    }
    if (token.kind == TK_INTEGER || token.kind == TK_REAL) {
        next(parser);
        return node(parser, N_VALUE, token);
    }
    if (accept(parser, TK_NAME)) return node(parser, N_NAME, token);
    fail(parser, "expected an expression");
    return NULL;
}

static Node *postfix(Parser *parser) {
    Node *left = primary(parser);
    while (parser->status == CT_OK && left) {
        int kind = parser->token.kind;
        if (kind == TK_INCREMENT || kind == TK_DECREMENT) {
            Node *result = node(parser, N_POSTFIX, parser->token);
            next(parser);
            if (!result) break;
            result->left = left;
            left = result;
        } else if (kind == '[') {
            Node *index = node(parser, N_INDEX, parser->token);
            next(parser);
            if (!index) break;
            index->left = left;
            index->right = comma_expression(parser);
            expect(parser, ']', "expected ']' after a subscript");
            left = index;
        } else if (kind == '.' || kind == TK_ARROW) {
            next(parser);
            Node *member = node(parser, N_MEMBER, parser->token);
            if (!member) break;
            member->through_pointer = kind == TK_ARROW;
            member->left = left;
            expect(parser, TK_NAME, "expected a member name");
            left = member;
        } else if (kind == '(') {
            Node *result = node(parser, N_CALL, left->token);
            next(parser);
            if (!result) break;
            result->left = left;
            Node **tail = &result->right;
            if (parser->token.kind != ')') {
                do {
                    *tail = expression(parser, 1);
                    if (!*tail) break;
                    tail = &(*tail)->next;
                } while (accept(parser, ','));
            }
            expect(parser, ')', "expected ')' after the arguments");
            left = result;
        } else break;
    }
    return left;
}

static Node *expression(Parser *parser, int minimum) {
    if (parser->status != CT_OK || !enter(parser)) return NULL;
    Node *left = postfix(parser);
    while (parser->status == CT_OK && precedence(parser->token.kind) >= minimum) {
        Token operator = parser->token;
        int level = precedence(operator.kind);
        next(parser);
        Node *result = node(parser, operator.kind == '?' ? N_CONDITIONAL : N_BINARY, operator);
        if (!result) break;
        result->left = left;
        if (operator.kind == '?') {
            result->right = comma_expression(parser);
            expect(parser, ':', "expected ':' in a conditional expression");
            result->third = expression(parser, 2);
        } else {
            if (is_assignment(operator.kind) && left && left->kind != N_NAME && left->kind != N_INDEX &&
                left->kind != N_MEMBER && left->kind != N_GENERIC &&
                !(left->kind == N_UNARY && left->token.kind == '*')) {
                fail(parser, "assignment requires an assignable expression on the left");
                parser->status = CT_ERROR;
            }
            result->right = expression(parser, is_assignment(operator.kind) ? level : level + 1);
        }
        left = result;
    }
    --parser->depth;
    return left;
}

/* Checked while parsing and leaves nothing to execute. */
/* C's full expression: assignment expressions joined by the comma operator. */
static Node *comma_expression(Parser *parser) {
    Node *left = expression(parser, 1);
    while (parser->status == CT_OK && left && parser->token.kind == ',') {
        Node *result = node(parser, N_COMMA, parser->token);
        next(parser);
        if (!result) break;
        result->left = left;
        result->right = expression(parser, 1);
        left = result;
    }
    return left;
}

static Node *static_assertion(Parser *parser) {
    Token keyword = parser->token;
    next(parser);
    expect(parser, '(', "expected '(' after _Static_assert");
    Node *condition = expression(parser, 1);
    expect(parser, ',', "expected ',' after the static assertion condition");
    Node *message = parser->token.kind == TK_STRING ? primary(parser) : NULL;
    if (!message) fail(parser, "expected a string literal message in _Static_assert");
    expect(parser, ')', "expected ')' after the static assertion message");
    expect(parser, ';', "expected ';' after the static assertion");
    if (parser->status != CT_OK) return NULL;
    int64_t value = 0;
    char text[sizeof parser->error->message];
    if (!constant_value(parser, condition, &value))
        (void)snprintf(text, sizeof text, "static assertion requires an integer constant expression");
    else if (!value)
        (void)snprintf(text, sizeof text, "static assertion failed: %s", message->text ? message->text : "");
    else return node(parser, N_EMPTY, keyword);
    fail_at(parser, keyword, text);
    parser->status = CT_ERROR;
    return NULL;
}

static Node *designated_item(Parser *parser) {
    Node *result = node(parser, N_DESIGNATED, parser->token);
    if (!result) return NULL;
    Node **chain = &result->left;
    while (parser->status == CT_OK && (parser->token.kind == '.' || parser->token.kind == '[')) {
        int field = parser->token.kind == '.';
        Node *designator = node(parser, N_DESIGNATOR, parser->token);
        next(parser);
        if (!designator) return result;
        designator->tag_kind = field;
        if (field) {
            designator->token = parser->token;
            expect(parser, TK_NAME, "expected a member name after '.'");
        } else {
            designator->left = expression(parser, 1);
            expect(parser, ']', "expected ']' after an array designator");
        }
        *chain = designator;
        chain = &designator->next;
    }
    expect(parser, '=', "expected '=' after a designator");
    result->right = initializer(parser);
    return result;
}

static Node *initializer(Parser *parser) {
    if (parser->token.kind != '{') return expression(parser, 1);
    Node *result = node(parser, N_INITIALIZER, parser->token);
    next(parser);
    if (!result) return NULL;
    Node **tail = &result->left;
    while (parser->status == CT_OK && parser->token.kind != '}') {
        Node *item = parser->token.kind == '.' || parser->token.kind == '[' ? designated_item(parser) : initializer(parser);
        if (!item) break;
        *tail = item;
        tail = &item->next;
        if (!accept(parser, ',')) break;
    }
    expect(parser, '}', "expected '}' after an initializer");
    return result;
}

static size_t initializer_extent(Parser *parser, Node *list) {
    size_t extent = 0, index = 0;
    for (Node *item = list->left; item; item = item->next, ++index) {
        if (item->kind == N_DESIGNATED) {
            Node *designator = item->left;
            int64_t value = 0;
            if (!designator || designator->tag_kind || !constant_value(parser, designator->left, &value) || value < 0) {
                fail(parser, "an array designator requires a non-negative integer constant expression");
                return 0;
            }
            index = (size_t)value;
        }
        if (index + 1 > extent) extent = index + 1;
    }
    return extent;
}

static Node *declaration(Parser *parser, int top_level) {
    int is_typedef = accept(parser, TK_TYPEDEF);
    int storage = 0, is_const = 0;
    Node *group = node(parser, N_GROUP, parser->token);
    if (!group) return NULL;
    Node **tail = &group->left;
    CtType base = type_specifier(parser, &storage, &is_const, &tail);
    if (parser->status != CT_OK) return group;
    if (!is_typedef && accept(parser, ';')) return group;
    do {
        Token name;
        Node *parameters = NULL;
        int variadic = 0;
        CtType type = declarator(parser, base, &name, &parameters, &variadic);
        if (parser->status != CT_OK) return group;
        if (!name.length) { fail(parser, "expected a name after the type"); return group; }
        if (is_typedef) {
            Node *alias = node(parser, N_TYPEDEF, name);
            if (!alias) return group;
            alias->type = type;
            alias->tag_kind = TAG_NAME;
            declare_alias(parser, alias);
            *tail = alias;
            tail = &alias->next;
            continue;
        }
        if (type_is_function(type)) {
            if (!top_level) { fail(parser, "function declarations require the top level"); return group; }
            Node *function = node(parser, N_FUNCTION, name);
            if (!function) return group;
            function->type = type;
            function->left = parameters;
            function->variadic = variadic;
            *tail = function;
            tail = &function->next;
            parser->unit->has_functions = 1;
            if (parser->token.kind == '{') {
                ++parser->functions;
                CtType enclosing = parser->function_type;
                parser->function_type = type_target(type);
                ++parser->scope;
                for (Node *parameter = parameters; parameter; parameter = parameter->next) declare_object(parser, parameter);
                function->right = statement(parser, 0);
                --parser->scope;
                pop_aliases(parser);
                parser->function_type = enclosing;
                --parser->functions;
                return group;
            }
            continue;
        }
        Node *result = node(parser, N_DECLARATION, name);
        if (!result) return group;
        result->type = type;
        result->is_static = storage == TK_STATIC;
        result->is_extern = storage == TK_EXTERN;
        result->is_const = is_const && !type_is_pointer(type);
        *tail = result;
        tail = &result->next;
        if (accept(parser, '=')) {
            result->left = initializer(parser);
            if (parser->status != CT_OK) return group;
            if (type_is_array(type) && !type_info(type)->count) {
                size_t extent = 0;
                if (result->left->kind == N_STRING) extent = result->left->text_length + 1;
                else if (result->left->kind == N_INITIALIZER) extent = initializer_extent(parser, result->left);
                if (!extent) { fail(parser, "an unsized array requires a brace or string initializer"); return group; }
                result->type = type_array(type_target(type), extent);
            }
        }
        if (!type_info(result->type)->complete || !ct_type_size(result->type)) {
            fail(parser, "a variable requires a complete object type");
            return group;
        }
        declare_object(parser, result);
    } while (accept(parser, ','));
    expect(parser, ';', "expected ';' after the declaration");
    return group;
}

static Node *statement_inner(Parser *parser, int top_level) {
    Token token = parser->token;
    if (token.kind == TK_STATIC_ASSERT) return static_assertion(parser);
    if (begins_type(parser, token)) return declaration(parser, top_level);
    if (token.kind == '{') {
        next(parser);
        Node *result = node(parser, N_BLOCK, token);
        if (!result) return NULL;
        ++parser->scope;
        Node **tail = &result->left;
        while (parser->status == CT_OK && parser->token.kind != '}' && parser->token.kind != TK_EOF) {
            *tail = statement(parser, 0);
            if (!*tail) break;
            tail = &(*tail)->next;
        }
        expect(parser, '}', "expected '}'");
        --parser->scope;
        pop_aliases(parser);
        return result;
    }
    if (accept(parser, TK_IF)) {
        Node *result = node(parser, N_IF, token);
        if (!result) return NULL;
        expect(parser, '(', "expected '(' after if");
        result->left = comma_expression(parser);
        expect(parser, ')', "expected ')' after the condition");
        result->right = statement(parser, 0);
        if (accept(parser, TK_ELSE)) result->third = statement(parser, 0);
        return result;
    }
    if (accept(parser, TK_DO)) {
        Node *result = node(parser, N_DO, token);
        if (!result) return NULL;
        ++parser->loops;
        result->right = statement(parser, 0);
        --parser->loops;
        expect(parser, TK_WHILE, "expected while after the do body");
        expect(parser, '(', "expected '(' after while");
        result->left = comma_expression(parser);
        expect(parser, ')', "expected ')' after the condition");
        expect(parser, ';', "expected ';' after do-while");
        return result;
    }
    if (accept(parser, TK_SWITCH)) {
        Node *result = node(parser, N_SWITCH, token);
        if (!result) return NULL;
        expect(parser, '(', "expected '(' after switch");
        result->left = comma_expression(parser);
        expect(parser, ')', "expected ')' after the switch expression");
        if (parser->token.kind != '{') fail(parser, "switch requires a block");
        ++parser->switches;
        result->right = statement(parser, 0);
        --parser->switches;
        return result;
    }
    if (token.kind == TK_CASE || token.kind == TK_DEFAULT) {
        if (!parser->switches) { fail(parser, "case outside a switch"); return NULL; }
        next(parser);
        Node *result = node(parser, N_CASE, token);
        if (!result) return NULL;
        if (token.kind == TK_CASE) result->left = expression(parser, 1);
        expect(parser, ':', "expected ':' after case");
        return result;
    }
    if (token.kind == TK_WHILE || token.kind == TK_FOR) {
        next(parser);
        Node *result = node(parser, token.kind == TK_WHILE ? N_WHILE : N_FOR, token);
        if (!result) return NULL;
        expect(parser, '(', "expected '(' after the loop keyword");
        ++parser->scope;
        if (token.kind == TK_FOR) {
            if (begins_type(parser, parser->token))
                result->left = declaration(parser, 0);
            else {
                if (parser->token.kind != ';') {
                    result->left = node(parser, N_EXPRESSION, parser->token);
                    if (result->left) result->left->left = comma_expression(parser);
                }
                expect(parser, ';', "expected ';' after the for initializer");
            }
            if (parser->token.kind != ';') result->right = comma_expression(parser);
            expect(parser, ';', "expected ';' after the for condition");
            if (parser->token.kind != ')') result->third = comma_expression(parser);
        } else result->left = comma_expression(parser);
        expect(parser, ')', "expected ')' after the loop header");
        ++parser->loops;
        if (token.kind == TK_FOR) result->fourth = statement(parser, 0);
        else result->right = statement(parser, 0);
        --parser->loops;
        --parser->scope;
        pop_aliases(parser);
        return result;
    }
    if (accept(parser, TK_GOTO)) {
        if (!parser->functions) { fail(parser, "goto outside a function"); return NULL; }
        Token label = parser->token;
        expect(parser, TK_NAME, "expected a label after goto");
        Node *result = node(parser, N_GOTO, label);
        expect(parser, ';', "expected ';' after goto");
        return result;
    }
    if (token.kind == TK_NAME) {
        Lexer lookahead = parser->lexer;
        if (lexer_next(&lookahead).kind == ':') {
            if (!parser->functions) { fail(parser, "label outside a function"); return NULL; }
            next(parser);
            next(parser);
            return node(parser, N_LABEL, token);
        }
    }
    if (token.kind == TK_RETURN || token.kind == TK_BREAK || token.kind == TK_CONTINUE) {
        if (token.kind == TK_RETURN ? !parser->functions :
            token.kind == TK_BREAK ? (!parser->loops && !parser->switches) : !parser->loops) {
            fail(parser, token.kind == TK_RETURN ? "return outside a function" : "loop control outside a loop");
            return NULL;
        }
        next(parser);
        NodeKind kind = token.kind == TK_RETURN ? N_RETURN : token.kind == TK_BREAK ? N_BREAK : N_CONTINUE;
        Node *result = node(parser, kind, token);
        if (!result) return NULL;
        if (kind == N_RETURN) {
            if (parser->function_type == CT_VOID && parser->token.kind != ';') fail(parser, "a void function cannot return a value");
            if (parser->function_type != CT_VOID && parser->token.kind == ';') fail(parser, "a non-void function must return a value");
            if (parser->token.kind != ';') result->left = comma_expression(parser);
        }
        expect(parser, ';', "expected ';' after the statement");
        return result;
    }
    if (accept(parser, ';')) return node(parser, N_EMPTY, token);
    Node *result = node(parser, N_EXPRESSION, token);
    if (!result) return NULL;
    result->left = comma_expression(parser);
    result->terminated = parser->token.kind == ';';
    if (!(top_level && parser->token.kind == TK_EOF))
        expect(parser, ';', "expected ';' after the expression");
    return result;
}

static Node *statement(Parser *parser, int top_level) {
    if (parser->status != CT_OK || !enter(parser)) return NULL;
    Node *result = statement_inner(parser, top_level);
    --parser->depth;
    return result;
}

void unit_destroy(Unit *unit) {
    if (!unit) return;
    Node *node_to_free = unit->allocations;
    while (node_to_free) {
        Node *next_node = node_to_free->allocated_next;
        free(node_to_free->text);
        node_to_free = next_node;
    }
    while (unit->node_blocks) {
        NodeBlock *next = unit->node_blocks->next;
        free(unit->node_blocks);
        unit->node_blocks = next;
    }
    free(unit->source);
    free(unit);
}

CtStatus parse_with_context(const char *source, const Unit *previous, Unit **unit, CtError *error) {
    *unit = NULL;
    size_t length = strlen(source);
    if (length > CT_SOURCE_LIMIT) {
        *error = (CtError){.line = 1, .column = 1, .message = "source size limit exceeded"};
        return CT_ERROR;
    }
    Unit *result = calloc(1, sizeof *result);
    if (result) result->source = malloc(length + 1);
    if (!result || !result->source) {
        unit_destroy(result);
        *error = (CtError){.line = 1, .column = 1, .message = "out of memory"};
        return CT_ERROR;
    }
    memcpy(result->source, source, length + 1);
    Parser parser = {.unit = result, .previous = previous, .error = error};
    lexer_init(&parser.lexer, result->source);
    next(&parser);
    Node **tail = &result->statements;
    while (parser.status == CT_OK && parser.token.kind != TK_EOF) {
        *tail = statement(&parser, 1);
        if (!*tail) break;
        tail = &(*tail)->next;
    }
    free_aliases(parser.aliases);
    free_aliases(parser.objects);
    if (parser.status != CT_OK) unit_destroy(result);
    else *unit = result;
    return parser.status;
}

CtStatus parse(const char *source, Unit **unit, CtError *error) {
    return parse_with_context(source, NULL, unit, error);
}

static void dump_node(FILE *output, const Node *node, unsigned depth) {
    static const char *names[] = {
        "value", "name", "unary", "postfix", "binary", "conditional", "call",
        "declaration", "expression", "block", "if", "while", "for", "return", "break",
        "continue", "function", "empty", "string", "index", "cast", "sizeof", "alignof",
        "declarations", "initializer", "do", "switch", "case", "goto", "label", "typedef",
        "enumerator", "generic", "association", "member", "compound", "designated", "designator",
        "va_start", "va_arg", "va_end", "va_copy", "comma"
    };
    for (; node; node = node->next) {
        fprintf(output, "%*s%s", (int)(depth * 2), "", names[node->kind]);
        if (node->token.length) fprintf(output, " %.*s", (int)node->token.length, node->token.start);
        if (node->kind == N_DECLARATION || node->kind == N_FUNCTION || node->kind == N_TYPEDEF ||
            node->kind == N_CAST || node->kind == N_COMPOUND || node->kind == N_VA_ARG) {
            char name[128];
            ct_type_name(node->type, name, sizeof name);
            fprintf(output, " : %s", name);
        }
        fprintf(output, " [%zu:%zu]\n", node->token.line, node->token.column);
        if (node->left) dump_node(output, node->left, depth + 1);
        if (node->right) dump_node(output, node->right, depth + 1);
        if (node->third) dump_node(output, node->third, depth + 1);
        if (node->fourth) dump_node(output, node->fourth, depth + 1);
    }
}

void dump_ast(const Unit *unit, FILE *output) {
    dump_node(output, unit->statements, 0);
}
