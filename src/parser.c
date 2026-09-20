#include "parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    Lexer lexer;
    Token token;
    Unit *unit;
    const Unit *previous;
    CtError *error;
    CtStatus status;
    size_t nodes;
    unsigned depth, loops, functions, switches;
    CtType function_type;
} Parser;

static void fail(Parser *parser, const char *message) {
    if (parser->status != CT_OK) return;
    parser->status = parser->token.kind == TK_EOF ? CT_INCOMPLETE : CT_ERROR;
    *parser->error = (CtError){.line = parser->token.line, .column = parser->token.column};
    (void)snprintf(parser->error->message, sizeof parser->error->message, "%s", message);
}

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

static Node *node(Parser *parser, NodeKind kind, Token token) {
    if (parser->status != CT_OK) return NULL;
    if (++parser->nodes > 16384) {
        fail(parser, "syntax tree size limit exceeded");
        parser->status = CT_ERROR;
        return NULL;
    }
    Node *result = calloc(1, sizeof *result);
    if (!result) {
        fail(parser, "out of memory");
        parser->status = CT_ERROR;
        return NULL;
    }
    result->kind = kind;
    result->token = token;
    result->allocated_next = parser->unit->allocations;
    parser->unit->allocations = result;
    return result;
}

int is_assignment(int kind) {
    return kind == '=' || (kind >= TK_ADD_ASSIGN && kind <= TK_SHR_ASSIGN);
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
static Node *statement(Parser *parser, int top_level);

static int enter(Parser *parser) {
    if (++parser->depth <= 128) return 1;
    fail(parser, "syntax nesting limit exceeded");
    parser->status = CT_ERROR;
    --parser->depth;
    return 0;
}

static Node *type_alias(Parser *parser, Token token, int enum_tag) {
    for (const Unit *unit = parser->unit; unit; unit = unit == parser->unit ? parser->previous : unit->next)
        for (Node *node = unit->allocations; node; node = node->allocated_next)
            if (node->kind == N_TYPEDEF && node->enum_tag == enum_tag && node->token.length == token.length &&
                !memcmp(node->token.start, token.start, token.length)) return node;
    return NULL;
}

static int begins_type(Parser *parser, Token token) {
    int kind = token.kind;
    return kind == TK_INT || kind == TK_DOUBLE || kind == TK_CHAR || kind == TK_VOID ||
           kind == TK_CONST || kind == TK_STATIC || kind == TK_TYPEDEF || kind == TK_ENUM ||
           (kind == TK_NAME && type_alias(parser, token, 0));
}

static CtType type_name(Parser *parser, int *is_static, int *is_const) {
    while (parser->token.kind == TK_STATIC || parser->token.kind == TK_CONST) {
        if (parser->token.kind == TK_STATIC) *is_static = 1;
        else *is_const = 1;
        next(parser);
    }
    CtType type = CT_INT;
    switch (parser->token.kind) {
        case TK_INT: type = CT_INT; break;
        case TK_DOUBLE: type = CT_DOUBLE; break;
        case TK_CHAR: type = CT_CHAR; break;
        case TK_VOID: type = CT_VOID; break;
        case TK_ENUM: {
            next(parser);
            Node *alias = type_alias(parser, parser->token, 1);
            if (!alias) { fail(parser, "unknown enum tag"); return CT_INT; }
            type = alias->type;
            break;
        }
        case TK_NAME: {
            Node *alias = type_alias(parser, parser->token, 0);
            if (!alias) { fail(parser, "unknown type name"); return CT_INT; }
            type = alias->type;
            break;
        }
        default: fail(parser, "expected a type"); return CT_INT;
    }
    next(parser);
    while (accept(parser, TK_CONST)) *is_const = 1;
    return type;
}

static CtType pointer_type(Parser *parser, CtType type) {
    unsigned depth = 0;
    while (accept(parser, '*')) {
        if (++depth > 16) { fail(parser, "pointer nesting limit exceeded"); break; }
        type = (CtType)(type + CT_POINTER);
        while (accept(parser, TK_CONST)) {}
    }
    return type;
}

static Node *primary(Parser *parser) {
    Token token = parser->token;
    if (accept(parser, TK_GENERIC)) {
        Node *result = node(parser, N_GENERIC, token);
        if (!result) return NULL;
        expect(parser, '(', "expected '(' after _Generic");
        result->left = expression(parser, 1);
        expect(parser, ',', "expected ',' after controlling expression");
        Node **tail = &result->right;
        do {
            Node *association = node(parser, N_ASSOCIATION, parser->token);
            if (!association) break;
            *tail = association;
            tail = &association->next;
            if (accept(parser, TK_DEFAULT)) association->is_const = 1;
            else {
                int is_static = 0, is_const = 0;
                CtType type = type_name(parser, &is_static, &is_const);
                association->type = pointer_type(parser, type);
            }
            expect(parser, ':', "expected ':' in generic association");
            association->left = expression(parser, 1);
        } while (accept(parser, ','));
        expect(parser, ')', "expected ')' after generic associations");
        return result;
    }
    if (accept(parser, '(')) {
        if (begins_type(parser, parser->token)) {
            int is_static = 0, is_const = 0;
            CtType type = type_name(parser, &is_static, &is_const);
            type = pointer_type(parser, type);
            expect(parser, ')', "expected ')' after type");
            Node *result = node(parser, N_CAST, token);
            if (result) { result->type = type; result->left = expression(parser, 13); }
            return result;
        }
        Node *result = expression(parser, 1);
        expect(parser, ')', "expected ')'");
        return result;
    }
    if (token.kind == TK_SIZEOF || token.kind == TK_ALIGNOF) {
        next(parser);
        Node *result = node(parser, token.kind == TK_SIZEOF ? N_SIZEOF : N_ALIGNOF, token);
        if (!result) return NULL;
        if (accept(parser, '(')) {
            if (begins_type(parser, parser->token)) {
                int is_static = 0, is_const = 0;
                CtType type = type_name(parser, &is_static, &is_const);
                result->type = pointer_type(parser, type);
            } else result->left = expression(parser, 1);
            expect(parser, ')', "expected ')' after sizeof operand");
        } else result->left = expression(parser, 13);
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
    if (accept(parser, TK_NAME)) {
        Node *result = node(parser, N_NAME, token);
        if (result && accept(parser, '(')) {
            result->kind = N_CALL;
            Node **tail = &result->left;
            if (parser->token.kind != ')') {
                do {
                    *tail = expression(parser, 1);
                    if (!*tail) break;
                    tail = &(*tail)->next;
                } while (accept(parser, ','));
            }
            expect(parser, ')', "expected ')' after arguments");
        }
        return result;
    }
    fail(parser, "expected an expression");
    return NULL;
}

static Node *expression(Parser *parser, int minimum) {
    if (parser->status != CT_OK || !enter(parser)) return NULL;
    Node *left = primary(parser);
    while (parser->status == CT_OK) {
        if (parser->token.kind == TK_INCREMENT || parser->token.kind == TK_DECREMENT) {
            Node *postfix = node(parser, N_POSTFIX, parser->token);
            next(parser);
            if (postfix) { postfix->left = left; left = postfix; }
        } else if (accept(parser, '[')) {
            Node *index = node(parser, N_INDEX, left->token);
            if (!index) break;
            index->left = left;
            index->right = expression(parser, 1);
            expect(parser, ']', "expected ']' after subscript");
            left = index;
        } else break;
    }
    while (parser->status == CT_OK && precedence(parser->token.kind) >= minimum) {
        Token operator = parser->token;
        int level = precedence(operator.kind);
        next(parser);
        Node *result = node(parser, operator.kind == '?' ? N_CONDITIONAL : N_BINARY, operator);
        if (!result) break;
        result->left = left;
        if (operator.kind == '?') {
            result->right = expression(parser, 1);
            expect(parser, ':', "expected ':' in conditional expression");
            result->third = expression(parser, 2);
        } else {
            if (is_assignment(operator.kind) && left && left->kind != N_NAME && left->kind != N_INDEX && left->kind != N_GENERIC &&
                !(left->kind == N_UNARY && left->token.kind == '*')) {
                fail(parser, "assignment requires a variable on the left");
                parser->status = CT_ERROR;
            }
            result->right = expression(parser, is_assignment(operator.kind) ? level : level + 1);
        }
        left = result;
    }
    --parser->depth;
    return left;
}

static Node *initializer(Parser *parser) {
    if (!accept(parser, '{')) return expression(parser, 1);
    Node *result = node(parser, N_INITIALIZER, parser->token);
    if (!result) return NULL;
    Node **tail = &result->left;
    while (parser->status == CT_OK && parser->token.kind != '}') {
        *tail = expression(parser, 1);
        if (!*tail) break;
        tail = &(*tail)->next;
        if (!accept(parser, ',')) break;
    }
    expect(parser, '}', "expected '}' after initializer");
    return result;
}

static Node *declaration(Parser *parser, int top_level) {
    int is_typedef = accept(parser, TK_TYPEDEF);
    if (is_typedef && !top_level) { fail(parser, "local typedefs are not implemented"); return NULL; }
    int is_static = 0, is_const = 0;
    CtType base = type_name(parser, &is_static, &is_const);
    Node *group = node(parser, N_GROUP, parser->token);
    if (!group) return NULL;
    Node **tail = &group->left;
    do {
        CtType type = pointer_type(parser, base);
        Token name = parser->token;
        expect(parser, TK_NAME, "expected a name after the type");
        Node *result = node(parser, is_typedef ? N_TYPEDEF : N_DECLARATION, name);
        if (!result) return group;
        result->type = type;
        result->is_static = is_static;
        result->is_const = is_const && type < CT_POINTER;
        *tail = result;
        tail = &result->next;
        if (accept(parser, '(')) {
            if (is_typedef) { fail(parser, "function typedefs are not implemented"); return group; }
            if (!top_level) { fail(parser, "function declarations require top level"); return group; }
            result->kind = N_FUNCTION;
            parser->unit->has_functions = 1;
            Node **parameter = &result->left;
            if (parser->token.kind == TK_VOID) {
                Lexer lookahead = parser->lexer;
                if (lexer_next(&lookahead).kind == ')') next(parser);
            }
            while (parser->status == CT_OK && parser->token.kind != ')') {
                int parameter_static = 0, parameter_const = 0;
                CtType parameter_type = type_name(parser, &parameter_static, &parameter_const);
                parameter_type = pointer_type(parser, parameter_type);
                if (parameter_type == CT_VOID) { fail(parser, "parameter cannot have void type"); break; }
                Token parameter_name = parser->token;
                if (parser->token.kind == TK_NAME) next(parser);
                else parameter_name.length = 0;
                *parameter = node(parser, N_DECLARATION, parameter_name);
                if (!*parameter) break;
                (*parameter)->type = parameter_type;
                if (accept(parser, '[')) {
                    expect(parser, ']', "only unsized parameter arrays are supported");
                    (*parameter)->type = (CtType)(parameter_type + CT_POINTER);
                }
                parameter = &(*parameter)->next;
                if (!accept(parser, ',')) break;
            }
            expect(parser, ')', "expected ')' after parameters");
            if (accept(parser, ';')) return group;
            if (parser->token.kind != '{') fail(parser, "expected function body or ';'");
            ++parser->functions;
            parser->function_type = result->type;
            result->right = statement(parser, 0);
            --parser->functions;
            return group;
        }
        if (is_typedef) {
            parser->unit->has_functions = 1;
            if (parser->token.kind != ',' && parser->token.kind != ';') fail(parser, "typedef requires a scalar or pointer type");
            continue;
        }
        if (type == CT_VOID) { fail(parser, "variable cannot have void type"); return group; }
        if (accept(parser, '[')) {
            result->is_array = 1;
            if (parser->token.kind != ']') result->right = expression(parser, 1);
            expect(parser, ']', "expected ']' after array size");
        }
        if (accept(parser, '=')) result->left = initializer(parser);
    } while (accept(parser, ','));
    expect(parser, ';', "expected ';' after declaration");
    return group;
}

static Node *enum_definition(Parser *parser, int top_level) {
    if (!top_level) { fail(parser, "local enum definitions are not implemented"); return NULL; }
    Token token = parser->token;
    next(parser);
    Node *group = node(parser, N_GROUP, token);
    if (!group) return NULL;
    Node **tail = &group->left;
    if (parser->token.kind == TK_NAME) {
        if (type_alias(parser, parser->token, 1)) { fail(parser, "enum tag is already defined"); return group; }
        *tail = node(parser, N_TYPEDEF, parser->token);
        if (!*tail) return group;
        (*tail)->enum_tag = 1;
        (*tail)->type = CT_INT;
        tail = &(*tail)->next;
        parser->unit->has_functions = 1;
        next(parser);
    }
    expect(parser, '{', "expected '{' after enum tag");
    Token previous = {0};
    while (parser->status == CT_OK && parser->token.kind != '}') {
        Token name = parser->token;
        expect(parser, TK_NAME, "expected enumerator name");
        *tail = node(parser, N_ENUMERATOR, name);
        if (!*tail) break;
        Node *enumerator = *tail;
        enumerator->type = CT_INT;
        enumerator->is_const = 1;
        if (accept(parser, '=')) enumerator->left = expression(parser, 1);
        else if (!previous.length) {
            enumerator->left = node(parser, N_VALUE, name);
            if (enumerator->left) enumerator->left->token.value = (CtValue){.type = CT_INT};
        } else {
            Token plus = name;
            plus.kind = '+';
            enumerator->left = node(parser, N_BINARY, plus);
            if (enumerator->left) {
                enumerator->left->left = node(parser, N_NAME, previous);
                enumerator->left->right = node(parser, N_VALUE, name);
                if (enumerator->left->right) enumerator->left->right->token.value = (CtValue){.type = CT_INT, .as.integer = 1};
            }
        }
        previous = name;
        tail = &enumerator->next;
        if (!accept(parser, ',')) break;
    }
    expect(parser, '}', "expected '}' after enumerators");
    expect(parser, ';', "expected ';' after enum definition");
    return group;
}

static Node *statement_inner(Parser *parser, int top_level) {
    Token token = parser->token;
    if (token.kind == TK_ENUM) {
        Lexer lookahead = parser->lexer;
        Token next_token = lexer_next(&lookahead);
        if (next_token.kind == TK_NAME) next_token = lexer_next(&lookahead);
        if (next_token.kind == '{') return enum_definition(parser, top_level);
    }
    if (begins_type(parser, token)) return declaration(parser, top_level);
    if (accept(parser, '{')) {
        Node *result = node(parser, N_BLOCK, token);
        if (!result) return NULL;
        Node **tail = &result->left;
        while (parser->status == CT_OK && parser->token.kind != '}' && parser->token.kind != TK_EOF) {
            *tail = statement(parser, 0);
            if (!*tail) break;
            tail = &(*tail)->next;
        }
        expect(parser, '}', "expected '}'");
        return result;
    }
    if (accept(parser, TK_IF)) {
        Node *result = node(parser, N_IF, token);
        if (!result) return NULL;
        expect(parser, '(', "expected '(' after if");
        result->left = expression(parser, 1);
        expect(parser, ')', "expected ')' after condition");
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
        expect(parser, TK_WHILE, "expected while after do body");
        expect(parser, '(', "expected '(' after while");
        result->left = expression(parser, 1);
        expect(parser, ')', "expected ')' after condition");
        expect(parser, ';', "expected ';' after do-while");
        return result;
    }
    if (accept(parser, TK_SWITCH)) {
        Node *result = node(parser, N_SWITCH, token);
        if (!result) return NULL;
        expect(parser, '(', "expected '(' after switch");
        result->left = expression(parser, 1);
        expect(parser, ')', "expected ')' after switch expression");
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
        expect(parser, '(', "expected '(' after loop keyword");
        if (token.kind == TK_FOR) {
            if (begins_type(parser, parser->token))
                result->left = declaration(parser, 0);
            else {
                if (parser->token.kind != ';') {
                    result->left = node(parser, N_EXPRESSION, parser->token);
                    if (result->left) result->left->left = expression(parser, 1);
                }
                expect(parser, ';', "expected ';' after for initializer");
            }
            if (parser->token.kind != ';') result->right = expression(parser, 1);
            expect(parser, ';', "expected ';' after for condition");
            if (parser->token.kind != ')') result->third = expression(parser, 1);
        } else result->left = expression(parser, 1);
        expect(parser, ')', "expected ')' after loop header");
        ++parser->loops;
        if (token.kind == TK_FOR) result->fourth = statement(parser, 0);
        else result->right = statement(parser, 0);
        --parser->loops;
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
            if (parser->function_type == CT_VOID && parser->token.kind != ';') fail(parser, "void function cannot return a value");
            if (parser->function_type != CT_VOID && parser->token.kind == ';') fail(parser, "non-void function must return a value");
            if (parser->token.kind != ';') result->left = expression(parser, 1);
        }
        expect(parser, ';', "expected ';' after statement");
        return result;
    }
    if (accept(parser, ';')) return node(parser, N_EMPTY, token);
    Node *result = node(parser, N_EXPRESSION, token);
    if (!result) return NULL;
    result->left = expression(parser, 1);
    result->terminated = parser->token.kind == ';';
    if (!(top_level && parser->token.kind == TK_EOF))
        expect(parser, ';', "expected ';' after expression");
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
        free(node_to_free);
        node_to_free = next_node;
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
        "declarations", "initializer", "do", "switch", "case", "goto", "label", "typedef", "enumerator", "generic", "association"
    };
    for (; node; node = node->next) {
        fprintf(output, "%*s%s", (int)(depth * 2), "", names[node->kind]);
        if (node->token.length) fprintf(output, " %.*s", (int)node->token.length, node->token.start);
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
