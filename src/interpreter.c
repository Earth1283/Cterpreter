#include "cterpreter.h"
#include "runtime.h"

#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(INT_MAX <= INT32_MAX, "Cterpreter requires int to be at most 32 bits");

typedef enum { FLOW_NORMAL, FLOW_RETURN, FLOW_BREAK, FLOW_CONTINUE, FLOW_GOTO } Flow;
typedef struct { Flow flow; CtValue value; int has_value; Token target; } Execution;
typedef struct { uint64_t address; CtType type; int readonly; } Lvalue;

static CtValue integer(int value) { return (CtValue){.type = CT_INT, .as.integer = value}; }
static CtValue real(double value) { return (CtValue){.type = CT_DOUBLE, .as.real = value}; }
static double as_real(CtValue value) { return value.type == CT_DOUBLE ? value.as.real : (double)value.as.integer; }
static int truth(CtValue value) { return type_is_pointer(value.type) ? value.as.address != 0 : as_real(value) != 0.0; }

static void scope_clear(CtInterpreter *interpreter, Scope *scope) {
    Symbol *symbol = scope->symbols;
    while (symbol) {
        Symbol *next = symbol->next;
        if (symbol->address && !symbol->is_static && !symbol->function)
            (void)memory_release(&interpreter->memory, symbol->address, 0);
        free(symbol->name);
        free(symbol);
        symbol = next;
    }
    scope->symbols = NULL;
    Temporary *temporary = scope->temporaries;
    while (temporary) {
        Temporary *next = temporary->next;
        (void)memory_release(&interpreter->memory, temporary->address, 0);
        free(temporary);
        temporary = next;
    }
    scope->temporaries = NULL;
}

CtInterpreter *ct_create(void) {
    CtInterpreter *interpreter = calloc(1, sizeof *interpreter);
    if (interpreter) {
        types_retain();
        interpreter->scope = &interpreter->globals;
        interpreter->input = stdin;
        interpreter->output = stdout;
        interpreter->errors = stderr;
        interpreter->step_limit = 1000000;
        interpreter->depth_limit = 256;
        interpreter->random_state = 1;
    }
    return interpreter;
}

void ct_clear(CtInterpreter *interpreter) {
    builtin_cleanup(interpreter);
    scope_clear(interpreter, &interpreter->globals);
    while (interpreter->functions) {
        FunctionRef *next = interpreter->functions->next;
        free(interpreter->functions);
        interpreter->functions = next;
    }
    while (interpreter->units) {
        Unit *next = interpreter->units->next;
        unit_destroy(interpreter->units);
        interpreter->units = next;
    }
    memory_destroy(&interpreter->memory);
    preprocessor_destroy(&interpreter->preprocessor);
    interpreter->scope = &interpreter->globals;
}

void ct_destroy(CtInterpreter *interpreter) {
    if (!interpreter) return;
    ct_clear(interpreter);
    free(interpreter);
    types_release();
}

void ct_set_interrupt(CtInterpreter *interpreter, const volatile sig_atomic_t *flag) {
    interpreter->interrupt = flag;
}

void ct_set_strict(CtInterpreter *interpreter, int strict) {
    interpreter->strict = strict;
}

CtValue runtime_error(CtInterpreter *interpreter, Token token, const char *message) {
    if (!interpreter->failed) {
        interpreter->failed = 1;
        *interpreter->error = (CtError){.line = token.line, .column = token.column};
        (void)snprintf(interpreter->error->message, sizeof interpreter->error->message, "%s", message);
    }
    return integer(0);
}

/* Strict mode turns an invalid access into the signal the program would have earned natively. */
static CtValue memory_error(CtInterpreter *interpreter, Token token) {
    const char *message = interpreter->memory.error ? interpreter->memory.error : "invalid memory access";
    if (interpreter->strict) {
        fprintf(interpreter->errors, "%s:%zu:%zu: %s\n",
                interpreter->filename ? interpreter->filename : "<stdin>", token.line, token.column, message);
        fflush(NULL);
        raise(SIGSEGV);
    }
    return runtime_error(interpreter, token, message);
}

static int tick(CtInterpreter *interpreter, Node *node) {
    if (interpreter->failed || interpreter->exit_requested) return 0;
    if (interpreter->interrupt && *interpreter->interrupt)
        (void)runtime_error(interpreter, node->token, "execution interrupted");
    else if (++interpreter->steps > interpreter->step_limit)
        (void)runtime_error(interpreter, node->token, "execution step limit exceeded");
    return !interpreter->failed;
}

static Symbol *lookup(Scope *scope, Token token, int local_only) {
    for (; scope; scope = local_only ? NULL : scope->parent)
        for (Symbol *symbol = scope->symbols; symbol; symbol = symbol->next)
            if (symbol->length == token.length && !memcmp(symbol->name, token.start, token.length))
                return symbol;
    return NULL;
}

static Symbol *define(CtInterpreter *interpreter, Token token, CtType type) {
    if (lookup(interpreter->scope, token, 1)) {
        (void)runtime_error(interpreter, token, "name already declared in this scope");
        return NULL;
    }
    Symbol *symbol = calloc(1, sizeof *symbol);
    if (symbol) symbol->name = malloc(token.length + 1);
    if (!symbol || !symbol->name) {
        free(symbol);
        (void)runtime_error(interpreter, token, "out of memory");
        return NULL;
    }
    memcpy(symbol->name, token.start, token.length);
    symbol->name[token.length] = '\0';
    symbol->length = token.length;
    symbol->value.type = type;
    symbol->next = interpreter->scope->symbols;
    interpreter->scope->symbols = symbol;
    return symbol;
}

static uint64_t temporary_object(CtInterpreter *interpreter, Scope *scope, Token token, size_t size) {
    Temporary *temporary = calloc(1, sizeof *temporary);
    if (!temporary) { (void)runtime_error(interpreter, token, "out of memory"); return 0; }
    temporary->address = memory_allocate(&interpreter->memory, size, 1, 0);
    if (!temporary->address) {
        free(temporary);
        (void)memory_error(interpreter, token);
        return 0;
    }
    temporary->next = scope->temporaries;
    scope->temporaries = temporary;
    return temporary->address;
}

static Node *function_at(CtInterpreter *interpreter, uint64_t address) {
    for (FunctionRef *reference = interpreter->functions; reference; reference = reference->next)
        if (reference->address == address) return reference->definition;
    return NULL;
}

CtValue runtime_convert(CtInterpreter *interpreter, Token token, CtValue value, CtType type) {
    if (type == CT_VOID) return (CtValue){.type = CT_VOID};
    if (value.type == CT_VOID) return runtime_error(interpreter, token, "void value used in an expression");
    if (type_is_aggregate(type) || type_is_aggregate(value.type)) {
        if (value.type != type) return runtime_error(interpreter, token, "aggregate types must match exactly");
        return value;
    }
    if (type_is_pointer(type)) {
        if (type_is_pointer(value.type)) return (CtValue){.type = type, .as.address = value.as.address};
        if (type_is_integer(value.type) && !value.as.integer) return (CtValue){.type = type};
        return runtime_error(interpreter, token, "pointer conversion requires a pointer or zero");
    }
    if (type_is_pointer(value.type)) return runtime_error(interpreter, token, "cannot convert a pointer to a number");
    if (type == CT_DOUBLE) return real(as_real(value));
    double truncated = trunc(as_real(value));
    if (!isfinite(truncated) || truncated < INT_MIN || truncated > INT_MAX)
        return runtime_error(interpreter, token, "conversion to int is out of range");
    CtValue result = integer((int)truncated);
    if (type == CT_CHAR) result = (CtValue){.type = CT_CHAR, .as.integer = (char)result.as.integer};
    return result;
}

static int base_operator(int kind) {
    switch (kind) {
        case TK_ADD_ASSIGN: return '+';
        case TK_SUB_ASSIGN: return '-';
        case TK_MUL_ASSIGN: return '*';
        case TK_DIV_ASSIGN: return '/';
        case TK_MOD_ASSIGN: return '%';
        case TK_AND_ASSIGN: return '&';
        case TK_OR_ASSIGN: return '|';
        case TK_XOR_ASSIGN: return '^';
        case TK_SHL_ASSIGN: return TK_SHL;
        case TK_SHR_ASSIGN: return TK_SHR;
        default: return kind;
    }
}

static CtValue binary(CtInterpreter *interpreter, Token token, CtValue left, CtValue right) {
    int kind = base_operator(token.kind);
    if (left.type == CT_VOID || right.type == CT_VOID)
        return runtime_error(interpreter, token, "operator cannot use a void value");
    if (type_is_aggregate(left.type) || type_is_aggregate(right.type))
        return runtime_error(interpreter, token, "operator cannot use an aggregate value");
    if (type_is_pointer(left.type) || type_is_pointer(right.type)) {
        if (!type_is_pointer(left.type) && kind == '+') { CtValue swap = left; left = right; right = swap; }
        if (type_is_pointer(left.type) && type_is_pointer(right.type)) {
            uint64_t a = left.as.address, b = right.as.address;
            if (kind == TK_EQ) return integer(a == b);
            if (kind == TK_NE) return integer(a != b);
            Allocation *object = memory_find(&interpreter->memory, a);
            if (!object || !object->alive || object != memory_find(&interpreter->memory, b))
                return runtime_error(interpreter, token, "pointer operation requires the same live object");
            if (kind == '<') return integer(a < b);
            if (kind == '>') return integer(a > b);
            if (kind == TK_LE) return integer(a <= b);
            if (kind == TK_GE) return integer(a >= b);
            if (kind == '-' && left.type == right.type) {
                size_t size = ct_type_size(type_target(left.type));
                if (!size) return runtime_error(interpreter, token, "arithmetic on void pointers is unsupported");
                int64_t difference = (int64_t)a - (int64_t)b;
                return integer((int)(difference / (int64_t)size));
            }
        } else if ((kind == TK_EQ || kind == TK_NE) &&
                   (type_is_pointer(left.type) ? type_is_integer(right.type) && right.as.integer == 0
                                               : type_is_integer(left.type) && left.as.integer == 0)) {
            int null = (type_is_pointer(left.type) ? left.as.address : right.as.address) == 0;
            return integer(kind == TK_EQ ? null : !null);
        } else if (type_is_pointer(left.type) && type_is_integer(right.type) && (kind == '+' || kind == '-')) {
            size_t size = ct_type_size(type_target(left.type));
            if (!size) return runtime_error(interpreter, token, "arithmetic on void pointers is unsupported");
            int64_t offset = (int64_t)right.as.integer * (int64_t)size;
            if (kind == '-') offset = -offset;
            Allocation *object = memory_find(&interpreter->memory, left.as.address);
            int64_t address = (int64_t)left.as.address + offset;
            if (!object || !object->alive || address < (int64_t)object->address ||
                address > (int64_t)(object->address + object->size))
                return runtime_error(interpreter, token, "pointer arithmetic outside object bounds");
            left.as.address = (uint64_t)address;
            return left;
        }
        return runtime_error(interpreter, token, "invalid pointer operands");
    }
    double a = as_real(left), b = as_real(right);
    switch (kind) {
        case TK_EQ: return integer(a == b);
        case TK_NE: return integer(a != b);
        case '<': return integer(a < b);
        case '>': return integer(a > b);
        case TK_LE: return integer(a <= b);
        case TK_GE: return integer(a >= b);
        default: break;
    }
    if (left.type == CT_DOUBLE || right.type == CT_DOUBLE) {
        double value = 0.0;
        switch (kind) {
            case '+': value = a + b; break;
            case '-': value = a - b; break;
            case '*': value = a * b; break;
            case '/':
                if (b == 0.0) return runtime_error(interpreter, token, "division by zero");
                value = a / b;
                break;
            default: return runtime_error(interpreter, token, "operator requires integer operands");
        }
        if (!isfinite(value)) return runtime_error(interpreter, token, "floating-point result is not finite");
        return real(value);
    }
    int64_t x = left.as.integer, y = right.as.integer, value = 0;
    switch (kind) {
        case '+': value = x + y; break;
        case '-': value = x - y; break;
        case '*': value = x * y; break;
        case '/': case '%':
            if (!y) return runtime_error(interpreter, token, "division by zero");
            if (x == INT_MIN && y == -1) return runtime_error(interpreter, token, "signed integer overflow");
            value = kind == '/' ? x / y : x % y;
            break;
        case '&': return integer(left.as.integer & right.as.integer);
        case '|': return integer(left.as.integer | right.as.integer);
        case '^': return integer(left.as.integer ^ right.as.integer);
        case TK_SHL: case TK_SHR:
            if (y < 0 || y >= (int64_t)(sizeof(int) * CHAR_BIT))
                return runtime_error(interpreter, token, "shift count is outside the int width");
            if (kind == TK_SHR) return integer(left.as.integer >> (unsigned)y);
            if (x < 0) return runtime_error(interpreter, token, "left shift of a negative value");
            value = x << (unsigned)y;
            break;
        default: return runtime_error(interpreter, token, "unsupported operator");
    }
    if (value < INT_MIN || value > INT_MAX) return runtime_error(interpreter, token, "signed integer overflow");
    return integer((int)value);
}

static CtValue evaluate(CtInterpreter *interpreter, Node *node);
static Execution execute(CtInterpreter *interpreter, Node *node);
static Execution sequence(CtInterpreter *interpreter, Node *head);
static CtType expression_type(CtInterpreter *interpreter, Node *node, unsigned depth);
static CtType object_type(CtInterpreter *interpreter, Node *node, unsigned depth);
static Lvalue lvalue(CtInterpreter *interpreter, Node *node);

static Node *generic_selection(CtInterpreter *interpreter, Node *node, unsigned depth) {
    CtType type = expression_type(interpreter, node->left, depth + 1);
    Node *selected = NULL, *fallback = NULL;
    for (Node *association = node->right; association; association = association->next) {
        for (Node *other = association->next; other; other = other->next)
            if ((association->is_const && other->is_const) ||
                (!association->is_const && !other->is_const && association->type == other->type)) {
                (void)runtime_error(interpreter, other->token, "duplicate generic association");
                return NULL;
            }
        if (association->is_const) fallback = association;
        else if (association->type == type) selected = association;
    }
    if (!selected) selected = fallback;
    if (!selected) (void)runtime_error(interpreter, node->token, "no matching generic association");
    return selected ? selected->left : NULL;
}

/* The type of the object an expression designates, before array or function decay. */
static CtType object_type(CtInterpreter *interpreter, Node *node, unsigned depth) {
    if (!node || depth > 128) return CT_VOID;
    switch (node->kind) {
        case N_STRING: return type_array(CT_CHAR, node->text_length + 1);
        case N_COMPOUND: return node->type;
        case N_NAME: {
            Symbol *symbol = lookup(interpreter->scope, node->token, 0);
            if (symbol) return symbol->value.type;
            break;
        }
        case N_INDEX: {
            CtType type = expression_type(interpreter, node->left, depth + 1);
            if (!type_is_pointer(type)) break;
            return type_target(type);
        }
        case N_UNARY: {
            if (node->token.kind != '*') break;
            CtType type = expression_type(interpreter, node->left, depth + 1);
            if (!type_is_pointer(type)) break;
            return type_target(type);
        }
        case N_MEMBER: {
            CtType type = expression_type(interpreter, node->left, depth + 1);
            if (node->through_pointer) {
                if (!type_is_pointer(type)) break;
                type = type_target(type);
            }
            const Member *member = type_member(type, node->token.start, node->token.length);
            if (!member) break;
            return member->type;
        }
        default: break;
    }
    return expression_type(interpreter, node, depth);
}

static CtType expression_type(CtInterpreter *interpreter, Node *node, unsigned depth) {
    if (!node || depth > 128) return CT_VOID;
    switch (node->kind) {
        case N_GENERIC: return expression_type(interpreter, generic_selection(interpreter, node, depth), depth + 1);
        case N_VALUE: return node->token.value.type;
        case N_STRING: return type_pointer(CT_CHAR);
        case N_CAST: return node->type;
        case N_COMPOUND: return type_decay(node->type);
        case N_SIZEOF: case N_ALIGNOF: return CT_INT;
        case N_INDEX: case N_MEMBER: return type_decay(object_type(interpreter, node, depth));
        case N_NAME: {
            Symbol *symbol = lookup(interpreter->scope, node->token, 0);
            if (symbol) return type_decay(symbol->value.type);
            if (node->token.length == 5 && !memcmp(node->token.start, "errno", 5)) return CT_INT;
            if ((node->token.length == 5 && !memcmp(node->token.start, "stdin", 5)) ||
                (node->token.length == 6 && (!memcmp(node->token.start, "stdout", 6) || !memcmp(node->token.start, "stderr", 6))))
                return type_pointer(CT_VOID);
            (void)runtime_error(interpreter, node->token, "unknown name");
            return CT_INT;
        }
        case N_CALL: {
            Node *callee = node->left;
            if (callee && callee->kind == N_NAME) {
                Symbol *symbol = lookup(interpreter->scope, callee->token, 0);
                CtType native;
                if (symbol && symbol->function) return type_target(symbol->function->type);
                if (!symbol && builtin_type(callee->token, &native)) return native;
            }
            CtType type = expression_type(interpreter, callee, depth + 1);
            if (type_is_pointer(type) && type_is_function(type_target(type))) return type_target(type_target(type));
            (void)runtime_error(interpreter, node->token, "call requires a function or a function pointer");
            return CT_INT;
        }
        case N_UNARY: case N_POSTFIX: {
            if (node->token.kind == '&') return type_pointer(object_type(interpreter, node->left, depth + 1));
            if (node->token.kind == '*') return type_decay(object_type(interpreter, node, depth));
            CtType type = expression_type(interpreter, node->left, depth + 1);
            if (node->token.kind == '!') return CT_INT;
            return type == CT_CHAR ? CT_INT : type;
        }
        case N_BINARY: {
            int op = node->token.kind;
            CtType left = expression_type(interpreter, node->left, depth + 1);
            if (is_assignment(op)) return left;
            if (op != '+' && op != '-' && op != '*' && op != '/') return CT_INT;
            CtType right = expression_type(interpreter, node->right, depth + 1);
            if (type_is_pointer(left) && type_is_pointer(right) && op == '-') return CT_INT;
            if (type_is_pointer(left)) return left;
            if (type_is_pointer(right)) return right;
            return left == CT_DOUBLE || right == CT_DOUBLE ? CT_DOUBLE : CT_INT;
        }
        case N_CONDITIONAL: {
            CtType a = expression_type(interpreter, node->right, depth + 1);
            CtType b = expression_type(interpreter, node->third, depth + 1);
            if (type_is_pointer(a) || type_is_aggregate(a)) return a;
            if (type_is_pointer(b) || type_is_aggregate(b)) return b;
            return a == CT_DOUBLE || b == CT_DOUBLE ? CT_DOUBLE : CT_INT;
        }
        default: return CT_INT;
    }
}

/* Types are needed to choose between aggregate copy and brace elision; errors are the caller's job. */
static CtType peek_type(CtInterpreter *interpreter, Node *node) {
    int failed = interpreter->failed;
    interpreter->failed = 1;
    CtType type = expression_type(interpreter, node, 0);
    interpreter->failed = failed;
    return type;
}

static CtValue read_value(CtInterpreter *interpreter, Token token, Lvalue object) {
    CtValue value = memory_read(&interpreter->memory, object.address, object.type);
    if (interpreter->memory.error) return memory_error(interpreter, token);
    return value;
}

/* Arrays, functions and aggregates are carried by address rather than by content. */
static CtValue load(CtInterpreter *interpreter, Token token, Lvalue object) {
    if (interpreter->failed) return integer(0);
    if (type_is_array(object.type))
        return (CtValue){.type = type_pointer(type_target(object.type)), .as.address = object.address};
    if (type_is_function(object.type))
        return (CtValue){.type = type_pointer(object.type), .as.address = object.address};
    if (type_is_aggregate(object.type)) return (CtValue){.type = object.type, .as.address = object.address};
    return read_value(interpreter, token, object);
}

static int copy_object(CtInterpreter *interpreter, Token token, uint64_t destination, uint64_t source, CtType type) {
    size_t size = ct_type_size(type);
    if (destination == source) return 1;
    void *from = memory_access(&interpreter->memory, source, size, 0);
    if (!from) { (void)memory_error(interpreter, token); return 0; }
    void *to = memory_access(&interpreter->memory, destination, size, 1);
    if (!to) { (void)memory_error(interpreter, token); return 0; }
    memmove(to, from, size);
    return 1;
}

static CtValue store_value(CtInterpreter *interpreter, Token token, Lvalue object, CtValue value) {
    if (interpreter->failed) return integer(0);
    if (object.readonly) return runtime_error(interpreter, token, "assignment to a const object");
    if (type_is_array(object.type) || type_is_function(object.type))
        return runtime_error(interpreter, token, "arrays and functions are not assignable");
    if (type_is_aggregate(object.type)) {
        if (value.type != object.type) return runtime_error(interpreter, token, "aggregate assignment requires the same type");
        if (!copy_object(interpreter, token, object.address, value.as.address, object.type)) return integer(0);
        return value;
    }
    value = runtime_convert(interpreter, token, value, object.type);
    if (!interpreter->failed && !memory_write(&interpreter->memory, object.address, value))
        return memory_error(interpreter, token);
    return value;
}

static void initialize(CtInterpreter *interpreter, uint64_t address, CtType type, Node **item, Token token);

static void initialize_list(CtInterpreter *interpreter, uint64_t address, CtType type, Node *list, Token token);

static int resolve_designator(CtInterpreter *interpreter, uint64_t base, CtType type, Node *chain,
                              uint64_t *address, CtType *result, size_t *index) {
    *address = base;
    *result = type;
    for (Node *designator = chain; designator && !interpreter->failed; designator = designator->next) {
        const TypeInfo *info = type_info(*result);
        if (designator->tag_kind) {
            if (info->kind != TY_STRUCT && info->kind != TY_UNION) {
                (void)runtime_error(interpreter, designator->token, "a member designator requires a struct or union");
                return 0;
            }
            const Member *member = type_member(*result, designator->token.start, designator->token.length);
            if (!member) { (void)runtime_error(interpreter, designator->token, "unknown member"); return 0; }
            if (designator == chain) *index = (size_t)(member - info->members);
            *address += member->offset;
            *result = member->type;
        } else {
            if (info->kind != TY_ARRAY) {
                (void)runtime_error(interpreter, designator->token, "an index designator requires an array");
                return 0;
            }
            CtValue position = evaluate(interpreter, designator->left);
            if (interpreter->failed) return 0;
            if (!type_is_integer(position.type) || position.as.integer < 0 ||
                (size_t)position.as.integer >= info->count) {
                (void)runtime_error(interpreter, designator->token, "array designator is outside the array bounds");
                return 0;
            }
            if (designator == chain) *index = (size_t)position.as.integer;
            *address += (size_t)position.as.integer * ct_type_size(info->target);
            *result = info->target;
        }
    }
    return !interpreter->failed;
}

static void initialize_list(CtInterpreter *interpreter, uint64_t address, CtType type, Node *list, Token token) {
    const TypeInfo *info = type_info(type);
    size_t index = 0;
    Node *item = list;
    while (item && !interpreter->failed) {
        uint64_t target = address;
        CtType target_type = type;
        if (item->kind == N_DESIGNATED) {
            if (!resolve_designator(interpreter, address, type, item->left, &target, &target_type, &index)) break;
            Node *value = item->right;
            initialize(interpreter, target, target_type, &value, item->token);
            item = item->next;
            ++index;
            continue;
        }
        if (info->kind == TY_ARRAY) {
            if (index >= info->count) { (void)runtime_error(interpreter, item->token, "too many initializers"); break; }
            target = address + index * ct_type_size(info->target);
            target_type = info->target;
        } else if (info->kind == TY_STRUCT || info->kind == TY_UNION) {
            if (index >= info->member_count) { (void)runtime_error(interpreter, item->token, "too many initializers"); break; }
            target = address + info->members[index].offset;
            target_type = info->members[index].type;
        } else if (index) { (void)runtime_error(interpreter, item->token, "too many initializers"); break; }
        initialize(interpreter, target, target_type, &item, token);
        ++index;
    }
}

static void initialize(CtInterpreter *interpreter, uint64_t address, CtType type, Node **item, Token token) {
    if (!*item || interpreter->failed) return;
    Node *value = *item;
    const TypeInfo *info = type_info(type);
    if (info->kind == TY_ARRAY && info->target == CT_CHAR && value->kind == N_STRING) {
        if (value->text_length + 1 > info->count && value->text_length > info->count) {
            (void)runtime_error(interpreter, value->token, "string initializer exceeds the array size");
            return;
        }
        size_t bytes = info->count > value->text_length ? value->text_length + 1 : info->count;
        void *target = memory_access(&interpreter->memory, address, bytes, 1);
        if (!target) { (void)memory_error(interpreter, value->token); return; }
        memcpy(target, value->text, bytes);
        *item = value->next;
        return;
    }
    if (value->kind == N_INITIALIZER) {
        initialize_list(interpreter, address, type, value->left, value->token);
        *item = value->next;
        return;
    }
    if (type_is_aggregate(type) && peek_type(interpreter, value) == type) {
        CtValue result = evaluate(interpreter, value);
        (void)store_value(interpreter, value->token, (Lvalue){address, type, 0}, result);
        *item = value->next;
        return;
    }
    if (info->kind == TY_ARRAY || info->kind == TY_STRUCT || info->kind == TY_UNION) {
        size_t count = info->kind == TY_ARRAY ? info->count : info->kind == TY_UNION ? 1 : info->member_count;
        for (size_t i = 0; i < count && *item && !interpreter->failed; ++i) {
            uint64_t target = info->kind == TY_ARRAY ? address + i * ct_type_size(info->target)
                                                     : address + info->members[i].offset;
            CtType member = info->kind == TY_ARRAY ? info->target : info->members[i].type;
            initialize(interpreter, target, member, item, token);
        }
        return;
    }
    CtValue result = evaluate(interpreter, value);
    (void)store_value(interpreter, value->token, (Lvalue){address, type, 0}, result);
    *item = value->next;
}

static Lvalue lvalue(CtInterpreter *interpreter, Node *node) {
    Lvalue result = {0};
    switch (node->kind) {
        case N_GENERIC: {
            Node *selected = generic_selection(interpreter, node, 0);
            return selected && !interpreter->failed ? lvalue(interpreter, selected) : result;
        }
        case N_NAME: {
            Symbol *symbol = lookup(interpreter->scope, node->token, 0);
            if (!symbol) { (void)runtime_error(interpreter, node->token, "unknown variable"); return result; }
            if (symbol->function) return (Lvalue){symbol->address, symbol->value.type, 1};
            return (Lvalue){symbol->address, symbol->value.type, symbol->is_const};
        }
        case N_COMPOUND: {
            uint64_t address = temporary_object(interpreter, interpreter->scope, node->token, ct_type_size(node->type));
            if (!address) return result;
            Node *item = node->left;
            initialize(interpreter, address, node->type, &item, node->token);
            return (Lvalue){address, node->type, 0};
        }
        case N_MEMBER: {
            CtValue object = evaluate(interpreter, node->left);
            if (interpreter->failed) return result;
            CtType type = object.type;
            if (node->through_pointer) {
                if (!type_is_pointer(type)) { (void)runtime_error(interpreter, node->token, "'->' requires a pointer"); return result; }
                type = type_target(type);
            }
            if (!type_is_aggregate(type)) {
                (void)runtime_error(interpreter, node->token, "member access requires a struct or union");
                return result;
            }
            const Member *member = type_member(type, node->token.start, node->token.length);
            if (!member) { (void)runtime_error(interpreter, node->token, "unknown member"); return result; }
            return (Lvalue){object.as.address + member->offset, member->type, 0};
        }
        case N_INDEX: case N_UNARY: {
            CtValue pointer;
            if (node->kind == N_UNARY) {
                if (node->token.kind != '*') { (void)runtime_error(interpreter, node->token, "expression is not assignable"); return result; }
                pointer = evaluate(interpreter, node->left);
            } else {
                pointer = evaluate(interpreter, node->left);
                CtValue offset = evaluate(interpreter, node->right);
                Token operator = node->token;
                operator.kind = '+';
                if (!interpreter->failed) pointer = binary(interpreter, operator, pointer, offset);
            }
            if (interpreter->failed) return result;
            if (!type_is_pointer(pointer.type)) {
                (void)runtime_error(interpreter, node->token, "dereference requires a pointer");
                return result;
            }
            return (Lvalue){pointer.as.address, type_target(pointer.type), 0};
        }
        default: (void)runtime_error(interpreter, node->token, "expression is not assignable"); return result;
    }
}

static CtValue call(CtInterpreter *interpreter, Node *node) {
    Node *callee = node->left;
    Node *function = NULL;
    Token name = callee && callee->kind == N_NAME ? callee->token : node->token;
    CtType native_type;
    int native = 0;
    if (callee && callee->kind == N_NAME) {
        Symbol *symbol = lookup(interpreter->scope, callee->token, 0);
        if (symbol && symbol->function) function = symbol->function;
        else if (!symbol) native = builtin_type(callee->token, &native_type);
    }
    if (!function && !native) {
        CtValue pointer = evaluate(interpreter, callee);
        if (interpreter->failed) return integer(0);
        if (!type_is_pointer(pointer.type) || !type_is_function(type_target(pointer.type)))
            return runtime_error(interpreter, name, "call requires a function or a function pointer");
        function = function_at(interpreter, pointer.as.address);
        if (!function) return runtime_error(interpreter, name, "call through an invalid function pointer");
    }
    if (function && !function->right) return runtime_error(interpreter, name, "function is declared but not defined");
    if (function && function->variadic) return runtime_error(interpreter, name, "variadic interpreted functions are unsupported");
    size_t parameters = 0, arguments = 0;
    if (function) for (Node *p = function->left; p; p = p->next) ++parameters;
    for (Node *a = node->right; a; a = a->next) ++arguments;
    if (!native && parameters != arguments) return runtime_error(interpreter, name, "incorrect number of arguments");
    CtValue *values = arguments ? malloc(arguments * sizeof *values) : NULL;
    if (arguments && !values) return runtime_error(interpreter, name, "out of memory");
    size_t index = 0;
    for (Node *a = node->right; a && !interpreter->failed; a = a->next)
        values[index++] = evaluate(interpreter, a);
    if (interpreter->failed) { free(values); return integer(0); }
    if (native) {
        CtValue result = builtin_call(interpreter, name, values, arguments);
        free(values);
        return result;
    }
    Scope *caller = interpreter->scope;
    Scope frame = {.parent = &interpreter->globals};
    interpreter->scope = &frame;
    index = 0;
    for (Node *p = function->left; p && !interpreter->failed; p = p->next) {
        Symbol *parameter = define(interpreter, p->token, p->type);
        if (!parameter) break;
        parameter->address = memory_allocate(&interpreter->memory, ct_type_size(p->type), 1, 0);
        if (!parameter->address) { (void)memory_error(interpreter, p->token); break; }
        (void)store_value(interpreter, p->token, (Lvalue){parameter->address, p->type, 0}, values[index++]);
    }
    free(values);
    Execution execution = {0};
    if (!interpreter->failed) execution = sequence(interpreter, function->right->left);
    CtType returns = type_target(function->type);
    CtValue result = integer(0);
    if (!interpreter->failed) {
        if (interpreter->exit_requested) result = integer(interpreter->exit_status);
        else if (execution.flow == FLOW_GOTO) result = runtime_error(interpreter, execution.target, "label is not reachable in this function scope");
        else if (returns == CT_VOID) result = (CtValue){.type = CT_VOID};
        else if (execution.flow != FLOW_RETURN && function->token.length == 4 && !memcmp(function->token.start, "main", 4)) result = integer(0);
        else if (execution.flow != FLOW_RETURN) result = runtime_error(interpreter, function->token, "function finished without returning a value");
        else result = runtime_convert(interpreter, name, execution.value, returns);
    }
    if (!interpreter->failed && type_is_aggregate(returns)) {
        uint64_t copy = temporary_object(interpreter, caller, name, ct_type_size(returns));
        if (copy && copy_object(interpreter, name, copy, result.as.address, returns)) result.as.address = copy;
    }
    scope_clear(interpreter, &frame);
    interpreter->scope = caller;
    return result;
}

static CtValue evaluate_inner(CtInterpreter *interpreter, Node *node) {
    switch (node->kind) {
        case N_VALUE: return node->token.value;
        case N_GENERIC: {
            Node *selected = generic_selection(interpreter, node, 0);
            return selected && !interpreter->failed ? evaluate(interpreter, selected) : integer(0);
        }
        case N_STRING: {
            if (!node->address) {
                node->address = memory_allocate(&interpreter->memory, node->text_length + 1, 1, 0);
                if (!node->address) return memory_error(interpreter, node->token);
                memcpy(memory_access(&interpreter->memory, node->address, node->text_length + 1, 1), node->text, node->text_length + 1);
                memory_find(&interpreter->memory, node->address)->readonly = 1;
            }
            return (CtValue){.type = type_pointer(CT_CHAR), .as.address = node->address};
        }
        case N_NAME: {
            Symbol *symbol = lookup(interpreter->scope, node->token, 0);
            if (!symbol) {
                CtValue value;
                if (builtin_value(interpreter, node->token, &value)) return value;
                return runtime_error(interpreter, node->token, "unknown variable");
            }
            return load(interpreter, node->token, lvalue(interpreter, node));
        }
        case N_INDEX: case N_MEMBER: case N_COMPOUND:
            return load(interpreter, node->token, lvalue(interpreter, node));
        case N_CAST: {
            CtValue value = evaluate(interpreter, node->left);
            return interpreter->failed ? integer(0) : runtime_convert(interpreter, node->token, value, node->type);
        }
        case N_SIZEOF: case N_ALIGNOF: {
            CtType type = node->left ? object_type(interpreter, node->left, 0) : node->type;
            if (interpreter->failed) return integer(0);
            size_t size = node->kind == N_SIZEOF ? ct_type_size(type) : ct_type_align(type);
            if (!size || !type_info(type)->complete)
                return runtime_error(interpreter, node->token, "sizeof requires a complete object type");
            return integer((int)size);
        }
        case N_CALL: return call(interpreter, node);
        case N_UNARY: case N_POSTFIX: {
            int op = node->token.kind;
            if (op == '&') {
                Lvalue object = lvalue(interpreter, node->left);
                if (interpreter->failed) return integer(0);
                return (CtValue){.type = type_pointer(object.type), .as.address = object.address};
            }
            if (op == '*') return load(interpreter, node->token, lvalue(interpreter, node));
            if (op == TK_INCREMENT || op == TK_DECREMENT) {
                Lvalue object = lvalue(interpreter, node->left);
                if (interpreter->failed) return integer(0);
                CtValue old = read_value(interpreter, node->token, object);
                if (interpreter->failed) return integer(0);
                Token operator = node->token;
                operator.kind = op == TK_INCREMENT ? '+' : '-';
                CtValue updated = binary(interpreter, operator, old, integer(1));
                updated = store_value(interpreter, node->token, object, updated);
                return node->kind == N_POSTFIX ? old : updated;
            }
            CtValue value = evaluate(interpreter, node->left);
            if (interpreter->failed) return integer(0);
            if (op == '!') return integer(!truth(value));
            if (!type_is_number(value.type)) return runtime_error(interpreter, node->token, "operator requires a numeric operand");
            if (value.type == CT_CHAR) value.type = CT_INT;
            if (op == '+') return value;
            if (op == '~') {
                if (value.type != CT_INT) return runtime_error(interpreter, node->token, "operator requires an integer operand");
                return integer(~value.as.integer);
            }
            if (value.type == CT_DOUBLE) return real(-value.as.real);
            if (value.as.integer == INT_MIN) return runtime_error(interpreter, node->token, "signed integer overflow");
            return integer(-value.as.integer);
        }
        case N_CONDITIONAL: {
            CtValue condition = evaluate(interpreter, node->left);
            if (interpreter->failed) return integer(0);
            CtValue value = evaluate(interpreter, truth(condition) ? node->right : node->third);
            return runtime_convert(interpreter, node->token, value, expression_type(interpreter, node, 0));
        }
        case N_BINARY: {
            int op = node->token.kind;
            if (is_assignment(op)) {
                Lvalue object = lvalue(interpreter, node->left);
                if (interpreter->failed) return integer(0);
                CtValue left = integer(0);
                if (op != '=') left = read_value(interpreter, node->token, object);
                if (interpreter->failed) return integer(0);
                CtValue value = evaluate(interpreter, node->right);
                if (!interpreter->failed && op != '=') value = binary(interpreter, node->token, left, value);
                return store_value(interpreter, node->token, object, value);
            }
            CtValue left = evaluate(interpreter, node->left);
            if (interpreter->failed) return integer(0);
            if (op == TK_AND && !truth(left)) return integer(0);
            if (op == TK_OR && truth(left)) return integer(1);
            CtValue right = evaluate(interpreter, node->right);
            if (interpreter->failed) return integer(0);
            if (op == TK_AND || op == TK_OR) return integer(truth(right));
            return binary(interpreter, node->token, left, right);
        }
        default: return runtime_error(interpreter, node->token, "expected an expression");
    }
}

static CtValue evaluate(CtInterpreter *interpreter, Node *node) {
    if (!tick(interpreter, node)) return integer(0);
    if (++interpreter->depth > interpreter->depth_limit) {
        --interpreter->depth;
        return runtime_error(interpreter, node->token, "evaluation nesting limit exceeded");
    }
    CtValue result = evaluate_inner(interpreter, node);
    --interpreter->depth;
    return result;
}

static Execution scoped(CtInterpreter *interpreter, Node *node) {
    Scope scope = {.parent = interpreter->scope};
    interpreter->scope = &scope;
    Execution result = execute(interpreter, node);
    interpreter->scope = scope.parent;
    scope_clear(interpreter, &scope);
    return result;
}

static int constant_expression(CtInterpreter *interpreter, Node *node) {
    if (!node) return 0;
    if (node->kind == N_VALUE || node->kind == N_SIZEOF || node->kind == N_ALIGNOF) return 1;
    if (node->kind == N_NAME) {
        Symbol *symbol = lookup(interpreter->scope, node->token, 0);
        return symbol && symbol->enum_constant;
    }
    if (node->kind == N_UNARY && strchr("+-!~", node->token.kind)) return constant_expression(interpreter, node->left);
    if (node->kind == N_CAST && (node->type == CT_INT || node->type == CT_CHAR)) return constant_expression(interpreter, node->left);
    if (node->kind == N_BINARY && !is_assignment(node->token.kind))
        return constant_expression(interpreter, node->left) && constant_expression(interpreter, node->right);
    if (node->kind == N_CONDITIONAL)
        return constant_expression(interpreter, node->left) && constant_expression(interpreter, node->right) &&
               constant_expression(interpreter, node->third);
    return 0;
}

static void declare_function(CtInterpreter *interpreter, Node *node) {
    for (Node *p = node->left; p && !interpreter->failed; p = p->next) {
        if (node->right && !p->token.length) (void)runtime_error(interpreter, p->token, "function definition requires parameter names");
        for (Node *q = p->next; q; q = q->next)
            if (p->token.length && p->token.length == q->token.length && !memcmp(p->token.start, q->token.start, p->token.length))
                (void)runtime_error(interpreter, q->token, "duplicate parameter name");
    }
    if (node->right && node->variadic) (void)runtime_error(interpreter, node->token, "variadic interpreted functions are unsupported");
    if (interpreter->failed) return;
    Symbol *symbol = lookup(interpreter->scope, node->token, 1);
    if (symbol) {
        Node *previous = symbol->function;
        if (!previous || symbol->value.type != node->type || (previous->right && node->right)) {
            (void)runtime_error(interpreter, node->token, "conflicting function declaration");
            return;
        }
    } else {
        symbol = define(interpreter, node->token, node->type);
        if (!symbol) return;
        symbol->address = memory_allocate(&interpreter->memory, 1, 1, 0);
        if (!symbol->address) { (void)memory_error(interpreter, node->token); return; }
        memory_find(&interpreter->memory, symbol->address)->readonly = 1;
        FunctionRef *reference = calloc(1, sizeof *reference);
        if (!reference) { (void)runtime_error(interpreter, node->token, "out of memory"); return; }
        reference->address = symbol->address;
        reference->next = interpreter->functions;
        interpreter->functions = reference;
    }
    if (!symbol->function || node->right) symbol->function = node;
    for (FunctionRef *reference = interpreter->functions; reference; reference = reference->next)
        if (reference->address == symbol->address && (!reference->definition || node->right))
            reference->definition = symbol->function;
}

static Execution execute_inner(CtInterpreter *interpreter, Node *node) {
    Execution result = {0};
    switch (node->kind) {
        case N_EMPTY: case N_TYPEDEF: break;
        case N_EXPRESSION:
            result.value = evaluate(interpreter, node->left);
            result.has_value = !interpreter->failed && result.value.type != CT_VOID && !node->terminated;
            break;
        case N_GROUP: result = sequence(interpreter, node->left); break;
        case N_DECLARATION: case N_ENUMERATOR: {
            if (node->kind == N_ENUMERATOR && !constant_expression(interpreter, node->left)) {
                (void)runtime_error(interpreter, node->token, "enumerator requires an integer constant expression");
                break;
            }
            Symbol *symbol = define(interpreter, node->token, node->type);
            if (!symbol) break;
            symbol->enum_constant = node->kind == N_ENUMERATOR;
            symbol->is_static = node->is_static;
            symbol->is_const = node->is_const;
            int zero = interpreter->scope == &interpreter->globals || node->is_static || node->left != NULL;
            int existing_static = node->is_static && node->address;
            symbol->address = existing_static ? node->address
                                              : memory_allocate(&interpreter->memory, ct_type_size(node->type), zero, 0);
            if (!symbol->address) { (void)memory_error(interpreter, node->token); break; }
            if (node->is_static) node->address = symbol->address;
            if (!existing_static && node->left) {
                Node *item = node->left;
                initialize(interpreter, symbol->address, node->type, &item, node->token);
                if (item && !interpreter->failed) (void)runtime_error(interpreter, item->token, "too many initializers");
            }
            if (node->is_const && !interpreter->failed) memory_find(&interpreter->memory, symbol->address)->readonly = 1;
            break;
        }
        case N_FUNCTION: declare_function(interpreter, node); break;
        case N_BLOCK: {
            Scope scope = {.parent = interpreter->scope};
            interpreter->scope = &scope;
            result = sequence(interpreter, node->left);
            interpreter->scope = scope.parent;
            scope_clear(interpreter, &scope);
            if (result.flow == FLOW_NORMAL) result.has_value = 0;
            break;
        }
        case N_IF: {
            CtValue condition = evaluate(interpreter, node->left);
            Node *branch = truth(condition) ? node->right : node->third;
            if (!interpreter->failed && branch) result = scoped(interpreter, branch);
            if (result.flow == FLOW_NORMAL) result.has_value = 0;
            break;
        }
        case N_CASE: case N_LABEL: break;
        case N_GOTO: result.flow = FLOW_GOTO; result.target = node->token; break;
        case N_SWITCH: {
            CtValue value = evaluate(interpreter, node->left);
            if (!type_is_integer(value.type)) { (void)runtime_error(interpreter, node->token, "switch requires an integer"); break; }
            Node *match = NULL, *fallback = NULL;
            for (Node *item = node->right->left; item && !interpreter->failed; item = item->next) {
                if (item->kind != N_CASE) continue;
                if (!item->left) {
                    if (fallback) { (void)runtime_error(interpreter, item->token, "duplicate default label"); break; }
                    fallback = item;
                } else {
                    if (!constant_expression(interpreter, item->left)) { (void)runtime_error(interpreter, item->token, "case requires an integer constant expression"); break; }
                    CtValue label = evaluate(interpreter, item->left);
                    if (!type_is_integer(label.type)) { (void)runtime_error(interpreter, item->token, "case requires an integer"); break; }
                    for (Node *previous = node->right->left; previous != item && !interpreter->failed; previous = previous->next) {
                        if (previous->kind != N_CASE || !previous->left) continue;
                        CtValue earlier = evaluate(interpreter, previous->left);
                        if (earlier.as.integer == label.as.integer)
                            (void)runtime_error(interpreter, item->token, "duplicate case label");
                    }
                    if (label.as.integer == value.as.integer) match = item;
                }
            }
            Scope scope = {.parent = interpreter->scope};
            interpreter->scope = &scope;
            if (!interpreter->failed) result = sequence(interpreter, match ? match : fallback);
            interpreter->scope = scope.parent;
            scope_clear(interpreter, &scope);
            if (result.flow == FLOW_BREAK || result.flow == FLOW_NORMAL) result = (Execution){0};
            break;
        }
        case N_DO:
            do {
                result = scoped(interpreter, node->right);
                if (interpreter->failed || interpreter->exit_requested || result.flow == FLOW_RETURN ||
                    result.flow == FLOW_GOTO || result.flow == FLOW_BREAK) break;
            } while (truth(evaluate(interpreter, node->left)));
            if (result.flow != FLOW_RETURN && result.flow != FLOW_GOTO) result = (Execution){0};
            break;
        case N_WHILE:
            while (!interpreter->failed && !interpreter->exit_requested && truth(evaluate(interpreter, node->left))) {
                if (interpreter->failed) break;
                result = scoped(interpreter, node->right);
                if (result.flow == FLOW_RETURN || result.flow == FLOW_GOTO || result.flow == FLOW_BREAK) break;
            }
            if (result.flow != FLOW_RETURN && result.flow != FLOW_GOTO) result = (Execution){0};
            break;
        case N_FOR: {
            Scope scope = {.parent = interpreter->scope};
            interpreter->scope = &scope;
            if (node->left) (void)execute(interpreter, node->left);
            while (!interpreter->failed && !interpreter->exit_requested) {
                if (node->right && !truth(evaluate(interpreter, node->right))) break;
                if (interpreter->failed) break;
                result = scoped(interpreter, node->fourth);
                if (result.flow == FLOW_RETURN || result.flow == FLOW_GOTO || result.flow == FLOW_BREAK || interpreter->failed) break;
                if (node->third) (void)evaluate(interpreter, node->third);
            }
            interpreter->scope = scope.parent;
            scope_clear(interpreter, &scope);
            if (result.flow != FLOW_RETURN && result.flow != FLOW_GOTO) result = (Execution){0};
            break;
        }
        case N_RETURN:
            result.value = node->left ? evaluate(interpreter, node->left) : (CtValue){.type = CT_VOID};
            result.flow = FLOW_RETURN;
            result.has_value = 1;
            break;
        case N_BREAK: result.flow = FLOW_BREAK; break;
        case N_CONTINUE: result.flow = FLOW_CONTINUE; break;
        default: (void)runtime_error(interpreter, node->token, "expected a statement"); break;
    }
    return result;
}

static Execution execute(CtInterpreter *interpreter, Node *node) {
    if (!tick(interpreter, node)) return (Execution){0};
    if (++interpreter->depth > interpreter->depth_limit) {
        --interpreter->depth;
        (void)runtime_error(interpreter, node->token, "execution nesting limit exceeded");
        return (Execution){0};
    }
    Execution result = execute_inner(interpreter, node);
    --interpreter->depth;
    return result;
}

static Execution sequence(CtInterpreter *interpreter, Node *head) {
    Execution result = {0};
    for (Node *node = head; node && !interpreter->failed && !interpreter->exit_requested;) {
        result = execute(interpreter, node);
        if (result.flow == FLOW_GOTO) {
            Node *label = head;
            while (label) {
                if (label->kind == N_LABEL && label->token.length == result.target.length &&
                    !memcmp(label->token.start, result.target.start, result.target.length)) break;
                label = label->next;
            }
            if (!label) break;
            node = label;
            result = (Execution){0};
        } else {
            if (result.flow != FLOW_NORMAL) break;
            node = node->next;
        }
    }
    return result;
}

CtStatus ct_eval(CtInterpreter *interpreter, const char *source,
                 CtValue *result, int *has_result, CtError *diagnostic) {
    *has_result = 0;
    *diagnostic = (CtError){0};
    Unit *unit = NULL;
    Preprocessor pending;
    if (!preprocessor_copy(&pending, &interpreter->preprocessor)) {
        *diagnostic = (CtError){.line = 1, .column = 1, .message = "out of memory"};
        return CT_ERROR;
    }
    char *processed = NULL;
    if (!preprocess(&pending, source, interpreter->filename ? interpreter->filename : "<stdin>", &processed, diagnostic)) {
        preprocessor_destroy(&pending);
        return strstr(diagnostic->message, "unterminated") ? CT_INCOMPLETE : CT_ERROR;
    }
    CtStatus status = parse_with_context(processed, interpreter->units, &unit, diagnostic);
    free(processed);
    if (status != CT_OK) { preprocessor_destroy(&pending); return status; }
    preprocessor_destroy(&interpreter->preprocessor);
    interpreter->preprocessor = pending;
    interpreter->error = diagnostic;
    interpreter->failed = 0;
    interpreter->steps = 0;
    interpreter->depth = 0;
    interpreter->exit_requested = 0;
    Execution execution = sequence(interpreter, unit->statements);
    status = interpreter->failed ? CT_ERROR : CT_OK;
    if (status == CT_OK) { *result = execution.value; *has_result = execution.has_value; }
    for (Node *node = unit->allocations; node; node = node->allocated_next)
        if (node->kind == N_STRING || node->is_static) unit->has_functions = 1;
    if (unit->has_functions) {
        unit->next = interpreter->units;
        interpreter->units = unit;
    } else unit_destroy(unit);
    return status;
}

void ct_format_value(CtValue value, char *buffer, size_t capacity) {
    if (!capacity) return;
    if (type_is_aggregate(value.type)) {
        char name[96];
        ct_type_name(value.type, name, sizeof name);
        (void)snprintf(buffer, capacity, "%s at 0x%llx", name, (unsigned long long)value.as.address);
    } else if (type_is_pointer(value.type)) (void)snprintf(buffer, capacity, "0x%llx", (unsigned long long)value.as.address);
    else if (value.type == CT_VOID) (void)snprintf(buffer, capacity, "void");
    else if (value.type != CT_DOUBLE) (void)snprintf(buffer, capacity, "%d", value.as.integer);
    else {
        (void)snprintf(buffer, capacity, "%.17g", value.as.real);
        size_t length = strlen(buffer);
        if (!strpbrk(buffer, ".eE") && length + 2 < capacity) {
            buffer[length] = '.';
            buffer[length + 1] = '0';
            buffer[length + 2] = '\0';
        }
    }
}

static size_t print_value(CtInterpreter *interpreter, CtValue value, char *buffer, size_t capacity, unsigned depth) {
    if (!capacity) return 0;
    const TypeInfo *info = type_info(value.type);
    if (depth > 8 || (info->kind != TY_STRUCT && info->kind != TY_UNION)) {
        ct_format_value(value, buffer, capacity);
        return strlen(buffer);
    }
    size_t used = 0;
    used += (size_t)snprintf(buffer, capacity, "{");
    for (size_t i = 0; i < info->member_count && used + 1 < capacity; ++i) {
        const Member *member = &info->members[i];
        int written = snprintf(buffer + used, capacity - used, "%s.%s = ", i ? ", " : "", member->name);
        if (written < 0) break;
        used += (size_t)written;
        if (used + 1 >= capacity) break;
        if (type_is_array(member->type) || type_is_function(member->type)) {
            used += (size_t)snprintf(buffer + used, capacity - used, "...");
            continue;
        }
        CtValue field = type_is_aggregate(member->type)
            ? (CtValue){.type = member->type, .as.address = value.as.address + member->offset}
            : memory_read(&interpreter->memory, value.as.address + member->offset, member->type);
        if (!type_is_aggregate(member->type) && interpreter->memory.error)
            used += (size_t)snprintf(buffer + used, capacity - used, "<uninitialized>");
        else used += print_value(interpreter, field, buffer + used, capacity - used, depth + 1);
    }
    if (used + 2 < capacity) { buffer[used++] = '}'; buffer[used] = '\0'; }
    return used;
}

void ct_print_value(CtInterpreter *interpreter, CtValue value, char *buffer, size_t capacity) {
    (void)print_value(interpreter, value, buffer, capacity, 0);
}

void ct_set_filename(CtInterpreter *interpreter, const char *filename) {
    interpreter->filename = filename;
}

void ct_set_streams(CtInterpreter *interpreter, FILE *input, FILE *output, FILE *errors) {
    interpreter->input = input ? input : stdin;
    interpreter->output = output ? output : stdout;
    interpreter->errors = errors ? errors : stderr;
}

void ct_set_limits(CtInterpreter *interpreter, size_t steps, unsigned depth) {
    interpreter->step_limit = steps ? steps : 1000000;
    interpreter->depth_limit = depth ? depth : 256;
}

unsigned ct_depth(CtInterpreter *interpreter) { return interpreter->depth; }

int ct_has_function(CtInterpreter *interpreter, const char *name) {
    Token token = {.start = name, .length = strlen(name)};
    Symbol *symbol = lookup(&interpreter->globals, token, 1);
    return symbol && symbol->function && symbol->function->right;
}

CtStatus ct_run_main(CtInterpreter *interpreter, int argc, const char *const *argv,
                     int *exit_status, CtError *diagnostic) {
    Token token = {.kind = TK_NAME, .start = "main", .length = 4, .line = 1, .column = 1};
    Symbol *symbol = lookup(&interpreter->globals, token, 1);
    *exit_status = 0;
    if (!symbol || !symbol->function || !symbol->function->right) return CT_OK;
    interpreter->error = diagnostic;
    interpreter->failed = 0;
    interpreter->depth = 0;
    interpreter->steps = 0;
    interpreter->exit_requested = 0;
    *diagnostic = (CtError){0};
    Node callee_node = {.kind = N_NAME, .token = token};
    Node call_node = {.kind = N_CALL, .token = token, .left = &callee_node};
    Node argc_node = {.kind = N_VALUE, .token = {.value = {.type = CT_INT, .as.integer = argc}}};
    Node argv_node = {.kind = N_VALUE};
    Node *parameter = symbol->function->left;
    if (type_target(symbol->function->type) != CT_INT) {
        (void)runtime_error(interpreter, token, "main must return int");
        return CT_ERROR;
    }
    if (parameter) {
        if (parameter->type != CT_INT || !parameter->next || parameter->next->next ||
            parameter->next->type != type_pointer(type_pointer(CT_CHAR))) {
            (void)runtime_error(interpreter, token, "main expects no parameters or (int, char **)");
            return CT_ERROR;
        }
        uint64_t arguments = memory_allocate(&interpreter->memory, ((size_t)argc + 1) * sizeof(uint64_t), 1, 0);
        if (!arguments) { (void)memory_error(interpreter, token); return CT_ERROR; }
        for (int i = 0; i < argc; ++i) {
            size_t length = strlen(argv[i]) + 1;
            uint64_t address = memory_allocate(&interpreter->memory, length, 1, 0);
            if (!address) { (void)memory_error(interpreter, token); return CT_ERROR; }
            memcpy(memory_access(&interpreter->memory, address, length, 1), argv[i], length);
            CtValue value = {.type = type_pointer(CT_CHAR), .as.address = address};
            (void)memory_write(&interpreter->memory, arguments + (size_t)i * sizeof(uint64_t), value);
        }
        argc_node.next = &argv_node;
        argv_node.token.value = (CtValue){.type = type_pointer(type_pointer(CT_CHAR)), .as.address = arguments};
        call_node.right = &argc_node;
    }
    CtValue result = call(interpreter, &call_node);
    if (!interpreter->failed) *exit_status = interpreter->exit_requested ? interpreter->exit_status : result.as.integer;
    return interpreter->failed ? CT_ERROR : CT_OK;
}

static CtStatus inspection_parse(CtInterpreter *interpreter, const char *source, Unit **unit, CtError *diagnostic) {
    Preprocessor pending;
    *diagnostic = (CtError){0};
    *unit = NULL;
    if (!preprocessor_copy(&pending, &interpreter->preprocessor)) {
        *diagnostic = (CtError){.line = 1, .column = 1, .message = "out of memory"};
        return CT_ERROR;
    }
    char *processed = NULL;
    int ok = preprocess(&pending, source, interpreter->filename ? interpreter->filename : "<stdin>", &processed, diagnostic);
    preprocessor_destroy(&pending);
    if (!ok) return CT_ERROR;
    CtStatus status = parse_with_context(processed, interpreter->units, unit, diagnostic);
    free(processed);
    return status;
}

CtStatus ct_dump_ast(CtInterpreter *interpreter, const char *source, FILE *output, CtError *error) {
    Unit *unit = NULL;
    CtStatus status = inspection_parse(interpreter, source, &unit, error);
    if (status == CT_OK) dump_ast(unit, output);
    unit_destroy(unit);
    return status;
}

CtStatus ct_inspect_type(CtInterpreter *interpreter, const char *source, CtType *type, CtError *diagnostic) {
    Unit *unit = NULL;
    *diagnostic = (CtError){0};
    CtStatus status = inspection_parse(interpreter, source, &unit, diagnostic);
    if (status != CT_OK) return status;
    interpreter->failed = 0;
    interpreter->error = diagnostic;
    if (!unit->statements || unit->statements->next || unit->statements->kind != N_EXPRESSION)
        (void)runtime_error(interpreter, (Token){.line = 1, .column = 1}, "type inspection requires one expression");
    else *type = object_type(interpreter, unit->statements->left, 0);
    unit_destroy(unit);
    return interpreter->failed ? CT_ERROR : CT_OK;
}

void ct_dump(CtInterpreter *interpreter, FILE *output) {
    for (Symbol *symbol = interpreter->globals.symbols; symbol; symbol = symbol->next) {
        char type[128], value[256];
        ct_type_name(symbol->value.type, type, sizeof type);
        if (symbol->function) {
            const TypeInfo *signature = type_info(symbol->value.type);
            ct_type_name(type_target(symbol->value.type), type, sizeof type);
            fprintf(output, "%s %s(", type, symbol->name);
            for (size_t i = 0; i < signature->parameter_count; ++i) {
                ct_type_name(signature->parameters[i], type, sizeof type);
                fprintf(output, "%s%s", i ? ", " : "", type);
            }
            if (signature->variadic) fprintf(output, ", ...");
            else if (!signature->parameter_count) fprintf(output, "void");
            fprintf(output, ")%s\n", symbol->function->right ? " { ... }" : ";");
        } else if (type_is_array(symbol->value.type) || type_is_aggregate(symbol->value.type)) {
            CtValue object = {.type = symbol->value.type, .as.address = symbol->address};
            ct_print_value(interpreter, object, value, sizeof value);
            fprintf(output, "%s %s = %s\n", type, symbol->name, type_is_array(symbol->value.type) ? "{ ... }" : value);
        } else {
            CtValue object = memory_read(&interpreter->memory, symbol->address, symbol->value.type);
            if (interpreter->memory.error) (void)snprintf(value, sizeof value, "<uninitialized>");
            else ct_format_value(object, value, sizeof value);
            fprintf(output, "%s %s = %s\n", type, symbol->name, value);
        }
    }
    fprintf(output, "Memory: %zu live bytes; limits: %zu steps, %u evaluation depth\n",
            interpreter->memory.bytes, interpreter->step_limit, interpreter->depth_limit);
}

int ct_exit_status(CtInterpreter *interpreter, int *status) {
    *status = interpreter->exit_status;
    return interpreter->exit_requested;
}
