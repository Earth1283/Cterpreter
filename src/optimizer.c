#include "runtime.h"

#include <stdlib.h>
#include <string.h>

/* Runs once over a freshly parsed unit, before any of it executes. Every
 * rewrite preserves what the program observes: constants are folded by the
 * evaluator itself, and a fold that would fail is left for run time, so the
 * diagnostic still appears when and where it would have. */

static int is_constant(const Node *node) { return node && node->kind == N_VALUE; }

static int decided_truth(const Node *node, int *truth) {
    if (!is_constant(node)) return 0;
    CtValue value = node->token.value;
    if (type_is_real(value.type)) *truth = value.as.real != 0.0;
    else if (type_is_integer(value.type) || type_is_pointer(value.type)) *truth = value.as.integer != 0;
    else return 0;
    return 1;
}

static int foldable(const Node *node) {
    switch (node->kind) {
        case N_UNARY:
            return (node->token.kind == '+' || node->token.kind == '-' || node->token.kind == '!' ||
                    node->token.kind == '~') && is_constant(node->left);
        case N_BINARY: {
            if (is_assignment(node->token.kind) || !is_constant(node->left)) return 0;
            int truth;
            /* The right operand of a decided && or || is never evaluated. */
            if ((node->token.kind == TK_AND || node->token.kind == TK_OR) && decided_truth(node->left, &truth) &&
                truth == (node->token.kind == TK_OR))
                return 1;
            return is_constant(node->right);
        }
        case N_CONDITIONAL:
            return is_constant(node->left) && is_constant(node->right) && is_constant(node->third);
        case N_CAST:
            return type_is_number(node->type) && is_constant(node->left);
        case N_COMMA:
            return is_constant(node->left) && is_constant(node->right);
        case N_SIZEOF: case N_ALIGNOF:
            return !node->left || is_constant(node->left) || node->left->kind == N_STRING;
        default: return 0;
    }
}

static void optimize_list(CtInterpreter *interpreter, Node **head, int top_level);
static void optimize_statement(CtInterpreter *interpreter, Node **slot);

static void optimize_nested(CtInterpreter *interpreter, Node *node, int depth) {
    for (; node; node = node->next) {
        /* Case labels and enumerators are checked for constancy at run time, on
         * their original form, and evaluated only once anyway. */
        if (node->kind == N_CASE || node->kind == N_ENUMERATOR) continue;
        if (node->kind == N_CALL) node->nesting = depth;
        optimize_nested(interpreter, node->left, depth + 1);
        optimize_nested(interpreter, node->right, depth + 1);
        optimize_nested(interpreter, node->third, depth + 1);
        CtValue value;
        if (foldable(node) && runtime_fold(interpreter, node, &value)) {
            node->kind = N_VALUE;
            node->token.value = value;
            node->left = node->right = node->third = NULL;
        }
    }
}

static void optimize_expression(CtInterpreter *interpreter, Node *node) { optimize_nested(interpreter, node, 1); }

/* Statements that may stand in for their enclosing if: nothing that declares a
 * name or can be the target of a jump. */
static int splice_safe(const Node *node) {
    switch (node->kind) {
        case N_DECLARATION: case N_GROUP: case N_ENUMERATOR: case N_TYPEDEF:
        case N_LABEL: case N_CASE: case N_FUNCTION:
            return 0;
        default: return 1;
    }
}

static void optimize_statement(CtInterpreter *interpreter, Node **slot) {
    Node *node = *slot;
    if (!node) return;
    switch (node->kind) {
        case N_BLOCK: optimize_list(interpreter, &node->left, 0); return;
        case N_GROUP:
            for (Node *item = node->left; item; item = item->next) {
                if (item->kind == N_FUNCTION) optimize_statement(interpreter, &item->right);
                else if (item->kind == N_DECLARATION) optimize_expression(interpreter, item->left);
            }
            return;
        case N_FUNCTION: optimize_statement(interpreter, &node->right); return;
        case N_EXPRESSION: case N_RETURN: optimize_expression(interpreter, node->left); return;
        case N_SWITCH:
            optimize_expression(interpreter, node->left);
            optimize_statement(interpreter, &node->right);
            return;
        case N_DO:
            optimize_expression(interpreter, node->left);
            optimize_statement(interpreter, &node->right);
            return;
        case N_WHILE: {
            optimize_expression(interpreter, node->left);
            optimize_statement(interpreter, &node->right);
            int truth;
            if (!decided_truth(node->left, &truth)) return;
            if (!truth) node->kind = N_EMPTY;
            else {
                node->kind = N_FOR;
                node->fourth = node->right;
                node->left = node->right = node->third = NULL;
            }
            return;
        }
        case N_FOR: {
            optimize_statement(interpreter, &node->left);
            optimize_expression(interpreter, node->right);
            optimize_expression(interpreter, node->third);
            optimize_statement(interpreter, &node->fourth);
            int truth;
            if (decided_truth(node->right, &truth) && truth) node->right = NULL;
            return;
        }
        case N_IF: {
            optimize_expression(interpreter, node->left);
            optimize_statement(interpreter, &node->right);
            optimize_statement(interpreter, &node->third);
            int truth;
            if (!decided_truth(node->left, &truth)) return;
            Node *branch = truth ? node->right : node->third;
            if (!branch) node->kind = N_EMPTY;
            else if (splice_safe(branch)) {
                branch->next = node->next;
                *slot = branch;
            }
            return;
        }
        default: return;
    }
}

static int removable(const Node *node) {
    return node->kind == N_EMPTY || node->kind == N_TYPEDEF || (node->kind == N_GROUP && !node->left);
}

static int jumps_away(const Node *node) {
    return node->kind == N_RETURN || node->kind == N_BREAK || node->kind == N_CONTINUE || node->kind == N_GOTO;
}

/* Inside a function, declaration groups are flattened into the enclosing list
 * and statements that can never run are dropped. The top level is left alone:
 * it runs once, and the last statement there supplies the REPL's result. */
static void optimize_list(CtInterpreter *interpreter, Node **head, int top_level) {
    Node **slot = head;
    int unreachable = 0;
    while (*slot) {
        optimize_statement(interpreter, slot);
        Node *node = *slot;
        if (top_level) { slot = &node->next; continue; }
        if (node->kind == N_LABEL || node->kind == N_CASE) unreachable = 0;
        if (unreachable || removable(node)) { *slot = node->next; continue; }
        if (node->kind == N_GROUP) {
            Node *last = node->left;
            while (last->next) last = last->next;
            last->next = node->next;
            *slot = node->left;
            continue;
        }
        unreachable = jumps_away(node);
        slot = &node->next;
    }
}

/* A function's scope chain at run time holds only its own declarations above
 * the globals, so a name it never declares always resolves to a global. A name
 * it does declare resolves to the declaration it lexically names, provided no
 * jump can enter a scope past a declaration: with goto, or a declaration
 * directly in a switch body, the scope chain decides as before. */
typedef struct {
    Node **declared, **visible;
    size_t declared_count, declared_capacity, visible_count, visible_capacity;
    size_t scope_start; /* where the innermost scope's entries begin in visible */
    int lexical, duplicated, failed;
} Binder;

static int same_name(const Node *a, const Node *b) {
    return a->token.length == b->token.length && !memcmp(a->token.start, b->token.start, a->token.length);
}

static void push(Binder *binder, Node ***items, size_t *count, size_t *capacity, Node *node) {
    if (*count == *capacity) {
        size_t grown = *capacity ? *capacity * 2 : 32;
        Node **resized = realloc(*items, grown * sizeof *resized);
        if (!resized) { binder->failed = 1; return; }
        *items = resized;
        *capacity = grown;
    }
    (*items)[(*count)++] = node;
}

static void survey(Binder *binder, Node *node) {
    for (; node; node = node->next) {
        if ((node->kind == N_DECLARATION || node->kind == N_ENUMERATOR) && node->token.length)
            push(binder, &binder->declared, &binder->declared_count, &binder->declared_capacity, node);
        if (node->kind == N_GOTO || node->kind == N_LABEL) binder->lexical = 0;
        if (node->kind == N_SWITCH && node->right)
            for (Node *item = node->right->left; item; item = item->next)
                if (item->kind == N_DECLARATION || item->kind == N_GROUP || item->kind == N_ENUMERATOR)
                    binder->lexical = 0;
        survey(binder, node->left);
        survey(binder, node->right);
        survey(binder, node->third);
        survey(binder, node->fourth);
    }
}

static void declare(Binder *binder, Node *node) {
    if (!node->token.length || (node->kind == N_DECLARATION && node->is_extern)) return;
    for (size_t i = binder->scope_start; i < binder->visible_count; ++i)
        if (same_name(binder->visible[i], node)) binder->duplicated = 1;
    push(binder, &binder->visible, &binder->visible_count, &binder->visible_capacity, node);
}

static void bind_use(Binder *binder, Node *use) {
    if (binder->lexical)
        for (size_t i = binder->visible_count; i-- > 0;)
            if (same_name(binder->visible[i], use)) {
                use->resolution = RESOLVE_LOCAL;
                use->cache.use.declaration = binder->visible[i];
                return;
            }
    for (size_t i = 0; i < binder->declared_count; ++i)
        if (same_name(binder->declared[i], use)) return;
    use->resolution = RESOLVE_GLOBAL;
}

static void mark_addressed(Node *node) {
    for (; node; node = node->next) {
        if (node->kind == N_NAME && node->resolution == RESOLVE_LOCAL) node->cache.use.declaration->unaddressed = -1;
        mark_addressed(node->left);
        mark_addressed(node->right);
        mark_addressed(node->third);
    }
}

static void bind_expression(Binder *binder, Node *node) {
    for (; node; node = node->next) {
        if (node->kind == N_NAME) bind_use(binder, node);
        bind_expression(binder, node->left);
        bind_expression(binder, node->right);
        bind_expression(binder, node->third);
        if (node->kind == N_UNARY && node->token.kind == '&') mark_addressed(node->left);
    }
}

static void bind_statement(Binder *binder, Node *node);

static void bind_scoped(Binder *binder, Node *node) {
    size_t mark = binder->visible_count, start = binder->scope_start;
    binder->scope_start = mark;
    bind_statement(binder, node);
    binder->visible_count = mark;
    binder->scope_start = start;
}

static void bind_statement(Binder *binder, Node *node) {
    if (!node) return;
    switch (node->kind) {
        case N_BLOCK: {
            size_t mark = binder->visible_count, start = binder->scope_start;
            binder->scope_start = mark;
            for (Node *item = node->left; item; item = item->next) bind_statement(binder, item);
            binder->visible_count = mark;
            binder->scope_start = start;
            return;
        }
        case N_GROUP:
            for (Node *item = node->left; item; item = item->next) bind_statement(binder, item);
            return;
        case N_DECLARATION: case N_ENUMERATOR:
            declare(binder, node);
            bind_expression(binder, node->left);
            return;
        case N_EXPRESSION: case N_RETURN: case N_CASE: bind_expression(binder, node->left); return;
        case N_IF:
            bind_expression(binder, node->left);
            bind_scoped(binder, node->right);
            bind_scoped(binder, node->third);
            return;
        case N_WHILE: case N_DO: case N_SWITCH:
            bind_expression(binder, node->left);
            bind_scoped(binder, node->right);
            return;
        case N_FOR: {
            size_t mark = binder->visible_count, start = binder->scope_start;
            binder->scope_start = mark;
            bind_statement(binder, node->left);
            bind_expression(binder, node->right);
            bind_expression(binder, node->third);
            bind_scoped(binder, node->fourth);
            binder->visible_count = mark;
            binder->scope_start = start;
            return;
        }
        default: return;
    }
}

/* When every use in a function is resolved lexically, nothing looks its
 * declarations up by name, so they need not be indexed in their scopes, nor
 * checked there for duplicates, which the binder has just ruled out. Variadic
 * parameters stay indexed for va_start's check. */
static void privatize(Binder *binder, Node *function) {
    int lexical = binder->lexical && !binder->duplicated;
    for (size_t i = 0; i < binder->declared_count; ++i) {
        Node *declaration = binder->declared[i];
        int eligible = lexical && declaration->unaddressed != -1 && declaration->kind == N_DECLARATION &&
                       !declaration->is_static && !declaration->is_extern && !declaration->is_const &&
                       type_is_scalar(declaration->type) && !(declaration->left && declaration->left->kind == N_INITIALIZER);
        declaration->unaddressed = eligible;
        if (lexical && !(declaration->kind == N_DECLARATION && declaration->is_extern)) declaration->resolution = RESOLVE_LOCAL;
    }
    if (function->variadic)
        for (Node *parameter = function->left; parameter; parameter = parameter->next) {
            parameter->resolution = RESOLVE_DYNAMIC;
            parameter->unaddressed = 0;
        }
}

static void bind_function(Binder *binder, Node *function) {
    binder->declared_count = binder->visible_count = binder->scope_start = 0;
    binder->lexical = 1;
    binder->duplicated = 0;
    survey(binder, function->left);
    survey(binder, function->right->left);
    /* The parameters and the body's outermost declarations share the call's scope. */
    for (Node *parameter = function->left; parameter; parameter = parameter->next) declare(binder, parameter);
    for (Node *item = function->right->left; item; item = item->next) bind_statement(binder, item);
    privatize(binder, function);
}

/* Whether evaluating anything here may create a temporary object in the
 * current scope. Nested blocks and for loops have scopes of their own. */
static int makes_temporaries(const Node *node);

static int any_makes_temporaries(const Node *list) {
    for (; list; list = list->next)
        if (makes_temporaries(list)) return 1;
    return 0;
}

static int makes_temporaries(const Node *node) {
    if (node->kind == N_BLOCK || node->kind == N_FOR) return 0;
    if (node->kind == N_CALL || node->kind == N_COMPOUND || node->kind == N_VA_ARG) return 1;
    return any_makes_temporaries(node->left) || any_makes_temporaries(node->right) ||
           any_makes_temporaries(node->third) || any_makes_temporaries(node->fourth);
}

static void mark_bare(Node *block) {
    for (Node *item = block->left; item; item = item->next)
        if (item->kind == N_DECLARATION || item->kind == N_GROUP || item->kind == N_ENUMERATOR ||
            item->kind == N_FUNCTION || makes_temporaries(item))
            return;
    block->bare = 1;
}

void optimize_unit(CtInterpreter *interpreter, Unit *unit) {
    optimize_list(interpreter, &unit->statements, 1);
    Binder binder = {0};
    for (Node *statement = unit->statements; statement && !binder.failed; statement = statement->next) {
        if (statement->kind != N_GROUP) continue;
        for (Node *item = statement->left; item && !binder.failed; item = item->next)
            if (item->kind == N_FUNCTION && item->right) bind_function(&binder, item);
    }
    free(binder.declared);
    free(binder.visible);
    for (Node *node = unit->allocations; node; node = node->allocated_next) {
        if (node->kind == N_BLOCK) mark_bare(node);
        runtime_prepare(node);
    }
}
