#include "cterpreter.h"
#include "runtime.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(INT_MAX <= INT32_MAX, "Cterpreter requires int to be at most 32 bits");

typedef enum { FLOW_NORMAL, FLOW_RETURN, FLOW_BREAK, FLOW_CONTINUE, FLOW_GOTO } Flow;
typedef struct { Flow flow; CtValue value; int has_value; Token target; } Execution;

static CtValue integer(int value) { return (CtValue){.type = CT_INT, .as.integer = value}; }
static CtValue real(double value) { return (CtValue){.type = CT_DOUBLE, .as.real = value}; }
static double as_real(CtValue value) { return value.type == CT_DOUBLE ? value.as.real : (double)value.as.integer; }
static int truth(CtValue value) { return value.type >= CT_POINTER ? value.as.address != 0 : as_real(value) != 0.0; }

static void scope_clear(CtInterpreter *interpreter, Scope *scope) {
    Symbol *symbol = scope->symbols;
    while (symbol) {
        Symbol *next = symbol->next;
        if (symbol->address && !symbol->is_static) (void)memory_release(&interpreter->memory, symbol->address, 0);
        free(symbol->name);
        free(symbol);
        symbol = next;
    }
    scope->symbols = NULL;
}

CtInterpreter *ct_create(void) {
    CtInterpreter *interpreter = calloc(1, sizeof *interpreter);
    if (interpreter) {
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
}

void ct_set_interrupt(CtInterpreter *interpreter, const volatile sig_atomic_t *flag) {
    interpreter->interrupt = flag;
}

CtValue runtime_error(CtInterpreter *interpreter, Token token, const char *message) {
    if (!interpreter->failed) {
        interpreter->failed = 1;
        *interpreter->error = (CtError){.line = token.line, .column = token.column};
        (void)snprintf(interpreter->error->message, sizeof interpreter->error->message, "%s", message);
    }
    return integer(0);
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

CtValue runtime_convert(CtInterpreter *interpreter, Token token, CtValue value, CtType type) {
    if (type == CT_VOID) return (CtValue){.type = CT_VOID};
    if (value.type == CT_VOID) return runtime_error(interpreter, token, "void value used in an expression");
    if (type >= CT_POINTER) {
        if (value.type >= CT_POINTER) return (CtValue){.type = type, .as.address = value.as.address};
        if (value.type != CT_DOUBLE && value.as.integer == 0) return (CtValue){.type = type};
        return runtime_error(interpreter, token, "pointer conversion requires a pointer or zero");
    }
    if (value.type >= CT_POINTER) return runtime_error(interpreter, token, "cannot convert a pointer to a number");
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
    if (left.type >= CT_POINTER || right.type >= CT_POINTER) {
        if (left.type < CT_POINTER && kind == '+') { CtValue swap = left; left = right; right = swap; }
        if (left.type >= CT_POINTER && right.type >= CT_POINTER) {
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
                size_t size = ct_type_size((CtType)(left.type - CT_POINTER));
                if (!size) return runtime_error(interpreter, token, "arithmetic on void pointers is unsupported");
                int64_t difference = (int64_t)a - (int64_t)b;
                return integer((int)(difference / (int64_t)size));
            }
        } else if ((kind == TK_EQ || kind == TK_NE) &&
                   (left.type >= CT_POINTER ? right.type != CT_DOUBLE && right.as.integer == 0 : left.type != CT_DOUBLE && left.as.integer == 0)) {
            int null = (left.type >= CT_POINTER ? left.as.address : right.as.address) == 0;
            return integer(kind == TK_EQ ? null : !null);
        } else if (left.type >= CT_POINTER && right.type != CT_DOUBLE && (kind == '+' || kind == '-')) {
            size_t size = ct_type_size((CtType)(left.type - CT_POINTER));
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

static CtType expression_type(CtInterpreter *interpreter, Node *node, unsigned depth) {
    if (!node || depth > 128) return CT_VOID;
    if (node->kind == N_GENERIC) return expression_type(interpreter, generic_selection(interpreter, node, depth), depth + 1);
    if (node->kind == N_VALUE) return node->token.value.type;
    if (node->kind == N_STRING) return (CtType)(CT_CHAR + CT_POINTER);
    if (node->kind == N_CAST) return node->type;
    if (node->kind == N_SIZEOF || node->kind == N_ALIGNOF) return CT_INT;
    if (node->kind == N_NAME || node->kind == N_CALL) {
        Symbol *symbol = lookup(interpreter->scope, node->token, 0);
        if (symbol) return symbol->is_array ? (CtType)(symbol->value.type + CT_POINTER) : symbol->value.type;
        if (node->kind == N_NAME) {
            if (node->token.length == 5 && !memcmp(node->token.start, "errno", 5)) return CT_INT;
            if ((node->token.length == 5 && !memcmp(node->token.start, "stdin", 5)) ||
                (node->token.length == 6 && (!memcmp(node->token.start, "stdout", 6) || !memcmp(node->token.start, "stderr", 6))))
                return (CtType)(CT_VOID + CT_POINTER);
        }
        CtType type;
        if (node->kind == N_CALL && builtin_type(node->token, &type)) return type;
        (void)runtime_error(interpreter, node->token, "unknown name");
        return CT_INT;
    }
    if (node->kind == N_INDEX) return (CtType)(expression_type(interpreter, node->left, depth + 1) - CT_POINTER);
    if (node->kind == N_UNARY || node->kind == N_POSTFIX) {
        CtType type = expression_type(interpreter, node->left, depth + 1);
        if (node->token.kind == '&') return (CtType)(type + CT_POINTER);
        if (node->token.kind == '*') return (CtType)(type - CT_POINTER);
        if (node->token.kind == '!') return CT_INT;
        return type == CT_CHAR ? CT_INT : type;
    }
    if (node->kind == N_BINARY) {
        int op = node->token.kind;
        CtType left = expression_type(interpreter, node->left, depth + 1);
        if (is_assignment(op)) return left;
        if (op != '+' && op != '-' && op != '*' && op != '/') return CT_INT;
        CtType right = expression_type(interpreter, node->right, depth + 1);
        if (left >= CT_POINTER && right >= CT_POINTER && op == '-') return CT_INT;
        if (left >= CT_POINTER) return left;
        if (right >= CT_POINTER) return right;
        return left == CT_DOUBLE || right == CT_DOUBLE ? CT_DOUBLE : CT_INT;
    }
    if (node->kind == N_CONDITIONAL) {
        CtType a = expression_type(interpreter, node->right, depth + 1);
        CtType b = expression_type(interpreter, node->third, depth + 1);
        if (a >= CT_POINTER) return a;
        if (b >= CT_POINTER) return b;
        return a == CT_DOUBLE || b == CT_DOUBLE ? CT_DOUBLE : CT_INT;
    }
    return CT_INT;
}

typedef struct { uint64_t address; CtType type; int readonly; } Lvalue;

static Lvalue lvalue(CtInterpreter *interpreter, Node *node) {
    Lvalue result = {0};
    if (node->kind == N_GENERIC) {
        Node *selected = generic_selection(interpreter, node, 0);
        return selected && !interpreter->failed ? lvalue(interpreter, selected) : result;
    }
    if (node->kind == N_NAME) {
        Symbol *symbol = lookup(interpreter->scope, node->token, 0);
        if (!symbol || symbol->function || symbol->is_array) {
            (void)runtime_error(interpreter, node->token, "expected a scalar variable");
            return result;
        }
        return (Lvalue){symbol->address, symbol->value.type, symbol->is_const};
    }
    CtValue pointer = integer(0);
    if (node->kind == N_UNARY && node->token.kind == '*') pointer = evaluate(interpreter, node->left);
    else if (node->kind == N_INDEX) {
        pointer = evaluate(interpreter, node->left);
        CtValue offset = evaluate(interpreter, node->right);
        Token operator = node->token;
        operator.kind = '+';
        if (!interpreter->failed) pointer = binary(interpreter, operator, pointer, offset);
    } else {
        (void)runtime_error(interpreter, node->token, "expression is not assignable");
        return result;
    }
    if (pointer.type < CT_POINTER) (void)runtime_error(interpreter, node->token, "dereference requires a pointer");
    if (interpreter->failed) return result;
    return (Lvalue){pointer.as.address, (CtType)(pointer.type - CT_POINTER), 0};
}

static CtValue read_value(CtInterpreter *interpreter, Token token, Lvalue object) {
    CtValue value = memory_read(&interpreter->memory, object.address, object.type);
    if (interpreter->memory.error) return runtime_error(interpreter, token, interpreter->memory.error);
    return value;
}

static CtValue store_value(CtInterpreter *interpreter, Token token, Lvalue object, CtValue value) {
    if (interpreter->failed) return integer(0);
    if (object.readonly) return runtime_error(interpreter, token, "assignment to a const object");
    value = runtime_convert(interpreter, token, value, object.type);
    if (!interpreter->failed && !memory_write(&interpreter->memory, object.address, value))
        return runtime_error(interpreter, token, interpreter->memory.error);
    return value;
}

static CtValue call(CtInterpreter *interpreter, Node *node) {
    Symbol *symbol = lookup(interpreter->scope, node->token, 0);
    CtType native_type;
    int native = !symbol && builtin_type(node->token, &native_type);
    if (!native && (!symbol || !symbol->function || !symbol->function->right))
        return runtime_error(interpreter, node->token, "unknown function or missing definition");
    Node *function = native ? NULL : symbol->function;
    size_t parameters = 0, arguments = 0;
    if (function) for (Node *p = function->left; p; p = p->next) ++parameters;
    for (Node *a = node->left; a; a = a->next) ++arguments;
    if (!native && parameters != arguments) return runtime_error(interpreter, node->token, "incorrect number of arguments");
    CtValue *values = arguments ? malloc(arguments * sizeof *values) : NULL;
    if (arguments && !values) return runtime_error(interpreter, node->token, "out of memory");
    size_t index = 0;
    for (Node *a = node->left; a && !interpreter->failed; a = a->next)
        values[index++] = evaluate(interpreter, a);
    if (interpreter->failed) { free(values); return integer(0); }
    if (native) {
        CtValue result = builtin_call(interpreter, node->token, values, arguments);
        free(values);
        return result;
    }
    Scope frame = {.parent = &interpreter->globals};
    Scope *caller = interpreter->scope;
    interpreter->scope = &frame;
    index = 0;
    for (Node *p = function->left; p && !interpreter->failed; p = p->next) {
        Symbol *parameter = define(interpreter, p->token, p->type);
        if (!parameter) break;
        parameter->address = memory_allocate(&interpreter->memory, ct_type_size(p->type), 0, 0);
        if (!parameter->address) { (void)runtime_error(interpreter, p->token, interpreter->memory.error); break; }
        (void)store_value(interpreter, p->token, (Lvalue){parameter->address, p->type, 0}, values[index++]);
    }
    free(values);
    Execution execution = {0};
    if (!interpreter->failed) execution = sequence(interpreter, function->right->left);
    CtValue result = integer(0);
    if (!interpreter->failed) {
        if (interpreter->exit_requested) result = integer(interpreter->exit_status);
        else if (execution.flow == FLOW_GOTO) result = runtime_error(interpreter, execution.target, "label is not reachable in this function scope");
        else if (function->type == CT_VOID) result = (CtValue){.type = CT_VOID};
        else if (execution.flow != FLOW_RETURN && function->token.length == 4 && !memcmp(function->token.start, "main", 4)) result = integer(0);
        else if (execution.flow != FLOW_RETURN && !interpreter->exit_requested)
            result = runtime_error(interpreter, function->token, "function finished without returning a value");
        else result = runtime_convert(interpreter, node->token, execution.value, function->type);
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
                if (!node->address) return runtime_error(interpreter, node->token, interpreter->memory.error);
                memcpy(memory_access(&interpreter->memory, node->address, node->text_length + 1, 1), node->text, node->text_length + 1);
                memory_find(&interpreter->memory, node->address)->readonly = 1;
            }
            return (CtValue){.type = (CtType)(CT_CHAR + CT_POINTER), .as.address = node->address};
        }
        case N_NAME: {
            Symbol *symbol = lookup(interpreter->scope, node->token, 0);
            if (!symbol) {
                CtValue value;
                if (builtin_value(interpreter, node->token, &value)) return value;
                return runtime_error(interpreter, node->token, "unknown variable");
            }
            if (symbol->function) return runtime_error(interpreter, node->token, "function requires a call");
            if (symbol->is_array) return (CtValue){.type = (CtType)(symbol->value.type + CT_POINTER), .as.address = symbol->address};
            return read_value(interpreter, node->token, (Lvalue){symbol->address, symbol->value.type, 0});
        }
        case N_INDEX: {
            Lvalue object = lvalue(interpreter, node);
            return interpreter->failed ? integer(0) : read_value(interpreter, node->token, object);
        }
        case N_CAST: {
            CtValue value = evaluate(interpreter, node->left);
            return interpreter->failed ? integer(0) : runtime_convert(interpreter, node->token, value, node->type);
        }
        case N_SIZEOF: case N_ALIGNOF: {
            size_t size;
            if (node->left && node->left->kind == N_STRING) size = node->left->text_length + 1;
            else if (node->left && node->left->kind == N_NAME) {
                Symbol *symbol = lookup(interpreter->scope, node->left->token, 0);
                if (!symbol) return runtime_error(interpreter, node->token, "unknown name in sizeof");
                size = ct_type_size(symbol->value.type) * (symbol->is_array && node->kind == N_SIZEOF ? symbol->count : 1);
            } else size = ct_type_size(node->left ? expression_type(interpreter, node->left, 0) : node->type);
            if (!size) return runtime_error(interpreter, node->token, "sizeof requires a complete object type");
            return integer((int)size);
        }
        case N_CALL: return call(interpreter, node);
        case N_UNARY: case N_POSTFIX: {
            int op = node->token.kind;
            if (op == '&') {
                Lvalue object = lvalue(interpreter, node->left);
                return (CtValue){.type = (CtType)(object.type + CT_POINTER), .as.address = object.address};
            }
            if (op == '*') {
                Lvalue object = lvalue(interpreter, node);
                return interpreter->failed ? integer(0) : read_value(interpreter, node->token, object);
            }
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
            if (value.type >= CT_POINTER || value.type == CT_VOID) return runtime_error(interpreter, node->token, "operator requires a numeric operand");
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
        return constant_expression(interpreter, node->left) && constant_expression(interpreter, node->right) && constant_expression(interpreter, node->third);
    return 0;
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
                (void)runtime_error(interpreter, node->token, "enumerator requires an integer constant expression"); break;
            }
            Symbol *symbol = define(interpreter, node->token, node->type);
            if (!symbol) break;
            symbol->enum_constant = node->kind == N_ENUMERATOR;
            symbol->is_array = node->is_array;
            symbol->is_static = node->is_static;
            symbol->is_const = node->is_const;
            size_t count = 1;
            if (node->is_array) {
                count = 0;
                if (node->right) {
                    CtValue size = evaluate(interpreter, node->right);
                    if (size.type != CT_INT || size.as.integer <= 0) (void)runtime_error(interpreter, node->token, "array size must be a positive int");
                    else count = (size_t)size.as.integer;
                } else if (node->left && node->left->kind == N_STRING && node->type == CT_CHAR) count = node->left->text_length + 1;
                else if (node->left && node->left->kind == N_INITIALIZER)
                    for (Node *item = node->left->left; item; item = item->next) ++count;
                if (!count) (void)runtime_error(interpreter, node->token, "array requires a size or initializer");
            }
            if (interpreter->failed) break;
            symbol->count = count;
            int zero = interpreter->scope == &interpreter->globals || node->is_static || (node->is_array && node->left);
            int existing_static = node->is_static && node->address;
            symbol->address = existing_static ? node->address : memory_allocate(&interpreter->memory, count * ct_type_size(node->type), zero, 0);
            if (!symbol->address) { (void)runtime_error(interpreter, node->token, interpreter->memory.error); break; }
            if (node->is_static) node->address = symbol->address;
            if (!existing_static && node->left) {
                if (node->is_array && node->left->kind == N_STRING && node->type == CT_CHAR) {
                    if (count < node->left->text_length) { (void)runtime_error(interpreter, node->token, "string initializer exceeds array size"); break; }
                    size_t bytes = count > node->left->text_length ? node->left->text_length + 1 : count;
                    memcpy(memory_access(&interpreter->memory, symbol->address, bytes, 1), node->left->text, bytes);
                } else if (node->left->kind == N_INITIALIZER) {
                    size_t index = 0;
                    for (Node *item = node->left->left; item && !interpreter->failed; item = item->next, ++index) {
                        if (index >= count) { (void)runtime_error(interpreter, item->token, "too many initializer elements"); break; }
                        CtValue value = evaluate(interpreter, item);
                        (void)store_value(interpreter, item->token, (Lvalue){symbol->address + index * ct_type_size(node->type), node->type, 0}, value);
                    }
                } else if (node->is_array) (void)runtime_error(interpreter, node->token, "array requires a brace or string initializer");
                else {
                    CtValue value = evaluate(interpreter, node->left);
                    if (node->kind == N_ENUMERATOR && value.type != CT_INT && value.type != CT_CHAR)
                        (void)runtime_error(interpreter, node->token, "enumerator requires an integer");
                    (void)store_value(interpreter, node->token, (Lvalue){symbol->address, node->type, 0}, value);
                }
            }
            if (node->is_const) memory_find(&interpreter->memory, symbol->address)->readonly = 1;
            break;
        }
        case N_FUNCTION: {
            for (Node *p = node->left; p && !interpreter->failed; p = p->next) {
                if (node->right && !p->token.length) (void)runtime_error(interpreter, p->token, "function definition requires parameter names");
                for (Node *q = p->next; q; q = q->next)
                    if (p->token.length && p->token.length == q->token.length && !memcmp(p->token.start, q->token.start, p->token.length))
                        (void)runtime_error(interpreter, q->token, "duplicate parameter name");
            }
            if (interpreter->failed) break;
            Symbol *symbol = lookup(interpreter->scope, node->token, 1);
            if (symbol) {
                Node *previous = symbol->function;
                if (!previous || previous->type != node->type || (previous->right && node->right)) {
                    (void)runtime_error(interpreter, node->token, "conflicting function declaration"); break;
                }
                Node *a = previous->left, *b = node->left;
                while (a && b && a->type == b->type) { a = a->next; b = b->next; }
                if (a || b) { (void)runtime_error(interpreter, node->token, "conflicting function parameters"); break; }
            } else symbol = define(interpreter, node->token, node->type);
            if (symbol && (!symbol->function || node->right)) symbol->function = node;
            break;
        }
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
            if (value.type != CT_INT && value.type != CT_CHAR) { (void)runtime_error(interpreter, node->token, "switch requires an integer"); break; }
            Node *match = NULL, *fallback = NULL;
            for (Node *item = node->right->left; item && !interpreter->failed; item = item->next) {
                if (item->kind != N_CASE) continue;
                if (!item->left) {
                    if (fallback) { (void)runtime_error(interpreter, item->token, "duplicate default label"); break; }
                    fallback = item;
                } else {
                    if (!constant_expression(interpreter, item->left)) { (void)runtime_error(interpreter, item->token, "case requires an integer constant expression"); break; }
                    CtValue label = evaluate(interpreter, item->left);
                    if (label.type != CT_INT && label.type != CT_CHAR) { (void)runtime_error(interpreter, item->token, "case requires an integer"); break; }
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
                if (interpreter->failed || interpreter->exit_requested || result.flow == FLOW_RETURN || result.flow == FLOW_GOTO || result.flow == FLOW_BREAK) break;
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
    if (value.type >= CT_POINTER) (void)snprintf(buffer, capacity, "0x%llx", (unsigned long long)value.as.address);
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

int ct_has_function(CtInterpreter *interpreter, const char *name) {
    Token token = {.start = name, .length = strlen(name)};
    Symbol *symbol = lookup(&interpreter->globals, token, 1);
    return symbol && symbol->function && symbol->function->right;
}

CtStatus ct_run_main(CtInterpreter *interpreter, int argc, const char *const *argv,
                     int *exit_status, CtError *diagnostic) {
    Token token = {.start = "main", .length = 4, .line = 1, .column = 1};
    Symbol *symbol = lookup(&interpreter->globals, token, 1);
    *exit_status = 0;
    if (!symbol || !symbol->function || !symbol->function->right) return CT_OK;
    interpreter->error = diagnostic;
    interpreter->failed = 0;
    interpreter->depth = 0;
    interpreter->steps = 0;
    interpreter->exit_requested = 0;
    *diagnostic = (CtError){0};
    Node call_node = {.kind = N_CALL, .token = token};
    Node argc_node = {.kind = N_VALUE, .token = {.value = {.type = CT_INT, .as.integer = argc}}};
    Node argv_node = {.kind = N_VALUE};
    Node *parameter = symbol->function->left;
    if (symbol->function->type != CT_INT) {
        (void)runtime_error(interpreter, token, "main must return int");
        return CT_ERROR;
    }
    uint64_t arguments = 0;
    if (parameter) {
        if (parameter->type != CT_INT || !parameter->next || parameter->next->next ||
            parameter->next->type != (CtType)(CT_CHAR + 2 * CT_POINTER)) {
            (void)runtime_error(interpreter, token, "main expects no parameters or (int, char **)");
            return CT_ERROR;
        }
        arguments = memory_allocate(&interpreter->memory, ((size_t)argc + 1) * sizeof(uint64_t), 1, 0);
        if (!arguments) { (void)runtime_error(interpreter, token, interpreter->memory.error); return CT_ERROR; }
        for (int i = 0; i < argc; ++i) {
            size_t length = strlen(argv[i]) + 1;
            uint64_t address = memory_allocate(&interpreter->memory, length, 1, 0);
            if (!address) { (void)runtime_error(interpreter, token, interpreter->memory.error); return CT_ERROR; }
            memcpy(memory_access(&interpreter->memory, address, length, 1), argv[i], length);
            CtValue value = {.type = (CtType)(CT_CHAR + CT_POINTER), .as.address = address};
            (void)memory_write(&interpreter->memory, arguments + (size_t)i * sizeof(uint64_t), value);
        }
        argc_node.next = &argv_node;
        argv_node.token.value = (CtValue){.type = (CtType)(CT_CHAR + 2 * CT_POINTER), .as.address = arguments};
        call_node.left = &argc_node;
    }
    CtValue result = call(interpreter, &call_node);
    if (!interpreter->failed) *exit_status = interpreter->exit_requested ? interpreter->exit_status : result.as.integer;
    return interpreter->failed ? CT_ERROR : CT_OK;
}

void ct_type_name(CtType type, char *buffer, size_t capacity) {
    unsigned pointers = (unsigned)type / CT_POINTER;
    CtType base = (CtType)((unsigned)type % CT_POINTER);
    const char *name = base == CT_INT ? "int" : base == CT_DOUBLE ? "double" : base == CT_CHAR ? "char" : "void";
    if (!capacity) return;
    (void)snprintf(buffer, capacity, "%s", name);
    size_t length = strlen(buffer);
    while (pointers-- && length + 1 < capacity) buffer[length++] = '*';
    buffer[length] = '\0';
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
    else *type = expression_type(interpreter, unit->statements->left, 0);
    unit_destroy(unit);
    return interpreter->failed ? CT_ERROR : CT_OK;
}

void ct_dump(CtInterpreter *interpreter, FILE *output) {
    for (Symbol *symbol = interpreter->globals.symbols; symbol; symbol = symbol->next) {
        char type[64], value[128];
        ct_type_name(symbol->value.type, type, sizeof type);
        if (symbol->function) {
            fprintf(output, "%s %s(", type, symbol->name);
            for (Node *parameter = symbol->function->left; parameter; parameter = parameter->next) {
                ct_type_name(parameter->type, type, sizeof type);
                fprintf(output, "%s %.*s%s", type, (int)parameter->token.length, parameter->token.start,
                        parameter->next ? ", " : "");
            }
            fprintf(output, ")%s\n", symbol->function->right ? " { ... }" : ";");
        } else if (symbol->is_array) fprintf(output, "%s %s[%zu]\n", type, symbol->name, symbol->count);
        else {
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
