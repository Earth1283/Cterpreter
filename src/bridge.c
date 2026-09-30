#define _POSIX_C_SOURCE 200809L

#include "runtime.h"
#include "boot.h"
#include "config.h"
#include "terminal.h"

#include <signal.h>
#include <stdlib.h>
#include <string.h>

volatile sig_atomic_t bridge_pending;
static volatile sig_atomic_t signal_arrived[SIGNAL_SLOTS];

static CtValue integer(int64_t value) { return (CtValue){.type = CT_INT, .as.integer = value}; }
static CtValue nothing(void) { return (CtValue){.type = CT_VOID}; }
static CtValue null_pointer(CtType target) { return (CtValue){.type = type_pointer(target)}; }

static uint64_t address_of(CtInterpreter *interpreter, Token name, CtValue value) {
    if (type_is_pointer(value.type)) return value.as.address;
    if (value.type == CT_INT && !value.as.integer) return 0;
    (void)runtime_error(interpreter, name, "library argument requires a pointer");
    return 0;
}

static int64_t integer_of(CtInterpreter *interpreter, Token name, CtValue value) {
    if (!type_is_integer(value.type)) {
        (void)runtime_error(interpreter, name, "library argument requires an integer");
        return 0;
    }
    return value.as.integer;
}

static void *window(CtInterpreter *interpreter, Token name, uint64_t address, size_t size, int write) {
    if (interpreter->failed) return NULL;
    void *block = memory_access(&interpreter->memory, address, size, write);
    if (!block) (void)runtime_error(interpreter, name, interpreter->memory.error);
    return block;
}

static int fetch(CtInterpreter *interpreter, Token name, uint64_t address, void *bytes, size_t size) {
    void *block = window(interpreter, name, address, size, 0);
    if (block) memcpy(bytes, block, size);
    return block != NULL;
}

static int put(CtInterpreter *interpreter, Token name, uint64_t address, const void *bytes, size_t size) {
    void *block = window(interpreter, name, address, size, 1);
    if (block) memcpy(block, bytes, size);
    return block != NULL;
}

static int put_int(CtInterpreter *interpreter, Token name, uint64_t address, int value) {
    return put(interpreter, name, address, &value, sizeof value);
}

static int put_text(CtInterpreter *interpreter, Token name, uint64_t address, size_t capacity, const char *text) {
    if (!capacity) return 1;
    size_t length = strlen(text);
    if (length >= capacity) length = capacity - 1;
    void *block = window(interpreter, name, address, length + 1, 1);
    if (!block) return 0;
    memcpy(block, text, length);
    ((char *)block)[length] = '\0';
    return 1;
}

static int require_size(CtInterpreter *interpreter, Token name, CtValue pointer, size_t size) {
    if (type_is_pointer(pointer.type) && type_size(type_target(pointer.type)) == size) return 1;
    (void)runtime_error(interpreter, name, "argument does not point to the structure this function expects");
    return 0;
}

static CtValue host_string(CtInterpreter *interpreter, Token name, const char *text) {
    if (!text) return null_pointer(CT_CHAR);
    for (size_t i = 0; i < interpreter->bridge_string_count; ++i)
        if (interpreter->bridge_strings[i].host == text)
            return (CtValue){.type = type_pointer(CT_CHAR), .as.address = interpreter->bridge_strings[i].address};
    CtValue copy = builtin_copy_string(interpreter, name, text);
    if (!interpreter->failed && interpreter->bridge_string_count < BRIDGE_STRINGS) {
        interpreter->bridge_strings[interpreter->bridge_string_count].host = text;
        interpreter->bridge_strings[interpreter->bridge_string_count++].address = copy.as.address;
    }
    return copy;
}

static Handle *find_handle(CtInterpreter *interpreter, uint64_t address, HandleKind kind) {
    for (Handle *handle = interpreter->handles; handle; handle = handle->next)
        if (handle->address == address && handle->kind == kind) return handle;
    return NULL;
}

static Handle *add_handle(CtInterpreter *interpreter, HandleKind kind, uint64_t address, void *object) {
    Handle *handle = calloc(1, sizeof *handle);
    if (!handle) return NULL;
    handle->address = address;
    handle->kind = kind;
    handle->object = object;
    handle->next = interpreter->handles;
    interpreter->handles = handle;
    return handle;
}

static void discard(CtInterpreter *interpreter, Handle *handle) {
    for (Handle **link = &interpreter->handles; *link; link = &(*link)->next)
        if (*link == handle) { *link = handle->next; break; }
    if (handle->kind == HANDLE_INTERPRETER) {
        ct_destroy(handle->object);
        (void)memory_release(&interpreter->memory, handle->address, 0);
    } else {
        terminal_destroy(handle->object);
        free(handle->object);
    }
    free(handle->text);
    free(handle);
}

static CtInterpreter *engine(CtInterpreter *interpreter, Token name, CtValue value, Handle **handle) {
    uint64_t address = address_of(interpreter, name, value);
    if (interpreter->failed) return NULL;
    Handle *found = find_handle(interpreter, address, HANDLE_INTERPRETER);
    if (!found) { (void)runtime_error(interpreter, name, "argument is not an interpreter created by ct_create"); return NULL; }
    if (handle) *handle = found;
    return found->object;
}

static size_t steps_left(const CtInterpreter *interpreter) {
    return interpreter->step_limit > interpreter->steps ? interpreter->step_limit - interpreter->steps : 1;
}

static unsigned frames_left(const CtInterpreter *interpreter) {
    return interpreter->depth_limit > interpreter->depth ? interpreter->depth_limit - interpreter->depth : 1;
}

static int fetch_value(CtInterpreter *interpreter, Token name, CtValue argument, CtValue *value) {
    if (!type_is_aggregate(argument.type) || type_size(argument.type) != sizeof *value) {
        (void)runtime_error(interpreter, name, "argument must be a CtValue");
        return 0;
    }
    return fetch(interpreter, name, argument.as.address, value, sizeof *value);
}

static CtValue status_result(CtStatus status) { return integer(status); }

static CtValue create_engine(CtInterpreter *interpreter, Token name) {
    if (interpreter->nesting + 1 >= interpreter->nesting_limit)
        return runtime_error(interpreter, name, "nested interpreter limit exceeded");
    CtInterpreter *child = ct_create();
    if (!child) return null_pointer(CT_VOID);
    uint64_t address = memory_allocate(&interpreter->memory, 1, 1, 0);
    Handle *handle = address ? add_handle(interpreter, HANDLE_INTERPRETER, address, child) : NULL;
    if (!handle) {
        ct_destroy(child);
        return address ? runtime_error(interpreter, name, "out of memory")
                       : runtime_error(interpreter, name, interpreter->memory.error);
    }
    ct_set_streams(child, interpreter->input, interpreter->output, interpreter->errors);
    ct_set_interrupt(child, interpreter->interrupt);
    ct_set_limits(child, steps_left(interpreter), frames_left(interpreter));
    ct_set_nesting(child, interpreter->nesting + 1, interpreter->nesting_limit);
    ct_set_strict(child, interpreter->strict);
    return (CtValue){.type = type_pointer(CT_VOID), .as.address = address};
}

static CtValue evaluate(CtInterpreter *interpreter, Token name, const CtValue *args) {
    CtInterpreter *child = engine(interpreter, name, args[0], NULL);
    char *source = child ? builtin_string(interpreter, name, args[1]) : NULL;
    uint64_t result_address = source ? address_of(interpreter, name, args[2]) : 0;
    uint64_t has_address = result_address ? address_of(interpreter, name, args[3]) : 0;
    uint64_t error_address = has_address ? address_of(interpreter, name, args[4]) : 0;
    if (interpreter->failed || !require_size(interpreter, name, args[2], sizeof(CtValue)) ||
        !require_size(interpreter, name, args[4], sizeof(CtError)))
        return integer(CT_ERROR);
    CtValue value = {.type = CT_VOID};
    CtError error = {0};
    int has_value = 0;
    CtStatus status = ct_eval(child, source, &value, &has_value, &error);
    if (put(interpreter, name, result_address, &value, sizeof value) &&
        put_int(interpreter, name, has_address, has_value))
        (void)put(interpreter, name, error_address, &error, sizeof error);
    return status_result(status);
}

static CtValue run_main(CtInterpreter *interpreter, Token name, const CtValue *args) {
    CtInterpreter *child = engine(interpreter, name, args[0], NULL);
    int64_t count = integer_of(interpreter, name, args[1]);
    uint64_t argv = address_of(interpreter, name, args[2]);
    uint64_t status_address = address_of(interpreter, name, args[3]);
    uint64_t error_address = address_of(interpreter, name, args[4]);
    if (interpreter->failed || !require_size(interpreter, name, args[4], sizeof(CtError))) return integer(CT_ERROR);
    if (count < 0 || count > 4096) return runtime_error(interpreter, name, "argument count is out of range");
    const char **vector = calloc((size_t)count + 1, sizeof *vector);
    if (!vector) return runtime_error(interpreter, name, "out of memory");
    for (int64_t i = 0; i < count && !interpreter->failed; ++i) {
        uint64_t item;
        if (!fetch(interpreter, name, argv + (uint64_t)i * sizeof item, &item, sizeof item)) break;
        vector[i] = memory_string(&interpreter->memory, item);
        if (!vector[i]) (void)runtime_error(interpreter, name, interpreter->memory.error);
    }
    CtStatus status = CT_ERROR;
    if (!interpreter->failed) {
        int exit_status = 0;
        CtError error = {0};
        status = ct_run_main(child, (int)count, vector, &exit_status, &error);
        if (put_int(interpreter, name, status_address, exit_status)) (void)put(interpreter, name, error_address, &error, sizeof error);
    }
    free(vector);
    return status_result(status);
}

static CtValue inspect_type(CtInterpreter *interpreter, Token name, const CtValue *args) {
    CtInterpreter *child = engine(interpreter, name, args[0], NULL);
    char *source = child ? builtin_string(interpreter, name, args[1]) : NULL;
    uint64_t type_address = source ? address_of(interpreter, name, args[2]) : 0;
    uint64_t error_address = type_address ? address_of(interpreter, name, args[3]) : 0;
    if (interpreter->failed || !require_size(interpreter, name, args[3], sizeof(CtError))) return integer(CT_ERROR);
    CtType type = CT_INT;
    CtError error = {0};
    CtStatus status = ct_inspect_type(child, source, &type, &error);
    int stored = status != CT_OK || put_int(interpreter, name, type_address, type);
    if (stored) (void)put(interpreter, name, error_address, &error, sizeof error);
    return status_result(status);
}

static CtValue parse_only(CtInterpreter *interpreter, Token name, const CtValue *args, size_t error_index, int dump) {
    CtInterpreter *child = engine(interpreter, name, args[0], NULL);
    char *source = child ? builtin_string(interpreter, name, args[1]) : NULL;
    FILE *stream = source && dump ? builtin_stream(interpreter, name, args[2]) : NULL;
    uint64_t error_address = source ? address_of(interpreter, name, args[error_index]) : 0;
    if (interpreter->failed || !require_size(interpreter, name, args[error_index], sizeof(CtError))) return integer(CT_ERROR);
    CtError error = {0};
    CtStatus status = dump ? ct_dump_ast(child, source, stream, &error) : ct_check(child, source, &error);
    (void)put(interpreter, name, error_address, &error, sizeof error);
    return status_result(status);
}

static CtValue lookup_text(CtInterpreter *interpreter, Token name, const CtValue *args, int completing) {
    CtInterpreter *child = engine(interpreter, name, args[0], NULL);
    char *text = child ? builtin_string(interpreter, name, args[1]) : NULL;
    int64_t length = text ? integer_of(interpreter, name, args[2]) : 0;
    int64_t index = completing ? integer_of(interpreter, name, args[3]) : 0;
    uint64_t buffer_address = address_of(interpreter, name, args[completing ? 4 : 3]);
    int64_t capacity = integer_of(interpreter, name, args[completing ? 5 : 4]);
    if (interpreter->failed) return integer(0);
    if (length < 0 || (size_t)length > strlen(text) || index < 0 || capacity < 0)
        return runtime_error(interpreter, name, "library argument is out of range");
    char buffer[1024];
    size_t room = (size_t)capacity < sizeof buffer ? (size_t)capacity : sizeof buffer;
    int found = completing ? ct_complete(child, text, (size_t)length, (size_t)index, buffer, room)
                           : ct_signature(child, text, (size_t)length, buffer, room);
    if (found && room) (void)put_text(interpreter, name, buffer_address, room, buffer);
    return integer(found);
}

static CtValue check_boot(CtInterpreter *interpreter, Token name) {
    CtValue slot = interpreter->result_slot;
    if (!type_is_aggregate(slot.type)) return runtime_error(interpreter, name, "boot_verify needs a BootCheck to return into");
    const Member *unit = type_member(slot.type, "unit", 4), *agreed = type_member(slot.type, "agreed", 6);
    if (!unit || !agreed) return runtime_error(interpreter, name, "boot_verify must return a BootCheck");
    BootCheck check = boot_verify();
    CtValue text = host_string(interpreter, name, check.unit);
    if (interpreter->failed) return integer(0);
    if (!memory_write(&interpreter->memory, slot.as.address + unit->offset, text) ||
        !memory_write(&interpreter->memory, slot.as.address + agreed->offset, integer(check.agreed)))
        return runtime_error(interpreter, name, interpreter->memory.error);
    return slot;
}

static int fetch_config(CtInterpreter *interpreter, Token name, CtValue argument, CliConfig *config) {
    return require_size(interpreter, name, argument, sizeof *config) &&
           fetch(interpreter, name, address_of(interpreter, name, argument), config, sizeof *config);
}

static int put_config(CtInterpreter *interpreter, Token name, CtValue argument, const CliConfig *config) {
    return put(interpreter, name, address_of(interpreter, name, argument), config, sizeof *config);
}

static int setting_index(CtInterpreter *interpreter, Token name, CtValue value) {
    int64_t index = integer_of(interpreter, name, value);
    if (!interpreter->failed && (index < 0 || index >= CFG_COUNT))
        (void)runtime_error(interpreter, name, "setting index is out of range");
    return (int)index;
}

static CtValue configuration(CtInterpreter *interpreter, Token name, size_t id, const CtValue *args) {
    CliConfig config;
    int index;
    char *first, *second;
    switch (id) {
        case BR_CONFIG_DEFAULTS:
            config_defaults(&config);
            return put_config(interpreter, name, args[0], &config) ? nothing() : integer(0);
        case BR_CONFIG_INDEX:
            first = builtin_string(interpreter, name, args[0]);
            return integer(first ? config_index(first) : -1);
        case BR_CONFIG_NAME: case BR_CONFIG_DESCRIPTION:
            index = setting_index(interpreter, name, args[0]);
            if (interpreter->failed) return integer(0);
            return host_string(interpreter, name, id == BR_CONFIG_NAME ? config_name(index) : config_description(index));
        case BR_CONFIG_VALUE:
            index = setting_index(interpreter, name, args[1]);
            if (interpreter->failed || !fetch_config(interpreter, name, args[0], &config)) return integer(0);
            return host_string(interpreter, name, config_value(&config, index));
        case BR_CONFIG_SET:
            first = builtin_string(interpreter, name, args[1]);
            second = first ? builtin_string(interpreter, name, args[2]) : NULL;
            if (!second || !fetch_config(interpreter, name, args[0], &config)) return integer(0);
            index = config_set(&config, first, second);
            return put_config(interpreter, name, args[0], &config) ? integer(index) : integer(0);
        default:
            first = builtin_string(interpreter, name, args[1]);
            if (!first || !fetch_config(interpreter, name, args[0], &config)) return integer(0);
            if (id == BR_CONFIG_SAVE) return integer(config_save(&config, first));
            index = config_load(&config, first, (int)integer_of(interpreter, name, args[2]));
            return put_config(interpreter, name, args[0], &config) ? integer(index) : integer(0);
    }
}

static struct {
    CtInterpreter *interpreter;
    Token name;
    CtValue session, hint, complete;
} active;

static const char *ask_session(CtValue function, const char *line, size_t cursor, char *copy, size_t capacity) {
    CtInterpreter *interpreter = active.interpreter;
    if (!function.as.address || interpreter->failed) return NULL;
    size_t size = strlen(line) + 1;
    uint64_t text = memory_allocate(&interpreter->memory, size, 0, 0);
    void *block = text ? memory_access(&interpreter->memory, text, size, 1) : NULL;
    if (!block) return NULL;
    memcpy(block, line, size);
    CtValue arguments[3] = {active.session, {.type = type_pointer(CT_CHAR), .as.address = text},
                            {.type = CT_ULONG, .as.unsigned_integer = cursor}};
    CtValue answer = runtime_invoke(interpreter, active.name, function, arguments, 3);
    (void)memory_release(&interpreter->memory, text, 0);
    if (interpreter->failed || !type_is_pointer(answer.type) || !answer.as.address) return NULL;
    const char *result = memory_string(&interpreter->memory, answer.as.address);
    if (!result) return NULL;
    (void)snprintf(copy, capacity, "%s", result);
    return copy;
}

static const char *session_hint(void *session, const char *line, size_t cursor) {
    static char copy[2048];
    (void)session;
    return ask_session(active.hint, line, cursor, copy, sizeof copy);
}

static const char *session_completion(void *session, const char *line, size_t cursor) {
    static char copy[2048];
    (void)session;
    return ask_session(active.complete, line, cursor, copy, sizeof copy);
}

static CtValue member_value(CtInterpreter *interpreter, CtType aggregate, uint64_t base, const char *field) {
    const Member *member = type_member(aggregate, field, strlen(field));
    if (!member) return (CtValue){.type = CT_VOID};
    CtValue value = memory_read(&interpreter->memory, base + member->offset, member->type);
    if (interpreter->memory.error) return (CtValue){.type = member->type};
    return value;
}

static CtType terminal_type(CtInterpreter *interpreter, Token name, CtValue argument) {
    if (type_is_pointer(argument.type) && type_is_aggregate(type_target(argument.type))) return type_target(argument.type);
    (void)runtime_error(interpreter, name, "argument must point to a Terminal");
    return CT_VOID;
}

static CtValue terminal_initialize(CtInterpreter *interpreter, Token name, const CtValue *args) {
    CtType type = terminal_type(interpreter, name, args[0]);
    uint64_t address = interpreter->failed ? 0 : address_of(interpreter, name, args[0]);
    uint64_t history = interpreter->failed ? 0 : address_of(interpreter, name, args[1]);
    char *path = history ? builtin_string(interpreter, name, args[1]) : NULL;
    int64_t color = interpreter->failed ? 0 : integer_of(interpreter, name, args[2]);
    if (interpreter->failed) return nothing();
    void *cleared = window(interpreter, name, address, type_size(type), 1);
    if (!cleared) return nothing();
    memset(cleared, 0, type_size(type));
    Handle *old = find_handle(interpreter, address, HANDLE_TERMINAL);
    if (old) discard(interpreter, old);
    Terminal *terminal = calloc(1, sizeof *terminal);
    Handle *handle = terminal ? add_handle(interpreter, HANDLE_TERMINAL, address, terminal) : NULL;
    if (!handle) { free(terminal); return runtime_error(interpreter, name, "out of memory"); }
    if (path && !(handle->text = strdup(path))) { discard(interpreter, handle); return runtime_error(interpreter, name, "out of memory"); }
    terminal_init(terminal, handle->text, (int)color);
    return nothing();
}

static CtValue terminal_line(CtInterpreter *interpreter, Token name, const CtValue *args) {
    CtType type = terminal_type(interpreter, name, args[0]);
    uint64_t address = interpreter->failed ? 0 : address_of(interpreter, name, args[0]);
    Handle *handle = address ? find_handle(interpreter, address, HANDLE_TERMINAL) : NULL;
    if (interpreter->failed) return null_pointer(CT_CHAR);
    if (!handle) return runtime_error(interpreter, name, "terminal_read requires a terminal set up by terminal_init");
    char *prompt = builtin_string(interpreter, name, args[1]);
    uint64_t flag_address = prompt ? address_of(interpreter, name, args[2]) : 0;
    const volatile sig_atomic_t *flag = &bridge_pending;
    if (interpreter->failed) return null_pointer(CT_CHAR);
    if (flag_address) {
        if (!window(interpreter, name, flag_address, sizeof(sig_atomic_t), 0)) return null_pointer(CT_CHAR);
        if (!interpreter->handlers_installed) flag = interpreter->interrupt;
    } else flag = interpreter->interrupt;
    Terminal *terminal = handle->object;
    terminal->color = (int)member_value(interpreter, type, address, "color").as.integer;
    terminal->highlighting = (int)member_value(interpreter, type, address, "highlighting").as.integer;
    terminal->suggestions = (int)member_value(interpreter, type, address, "suggestions").as.integer;
    CtValue context = member_value(interpreter, type, address, "context");
    terminal->context = type_is_pointer(context.type) && context.as.address ? memory_string(&interpreter->memory, context.as.address) : NULL;
    interpreter->memory.error = NULL;
    active.interpreter = interpreter;
    active.name = name;
    active.session = member_value(interpreter, type, address, "session");
    active.hint = member_value(interpreter, type, address, "hint");
    active.complete = member_value(interpreter, type, address, "complete");
    terminal->hint = type_is_pointer(active.hint.type) && active.hint.as.address ? session_hint : NULL;
    terminal->complete = type_is_pointer(active.complete.type) && active.complete.as.address ? session_completion : NULL;
    char *line = terminal_read(terminal, prompt, flag);
    terminal->context = NULL;
    if (!line) return null_pointer(CT_CHAR);
    size_t size = strlen(line) + 1;
    uint64_t copy = interpreter->failed ? 0 : memory_allocate(&interpreter->memory, size, 0, 1);
    void *block = copy ? memory_access(&interpreter->memory, copy, size, 1) : NULL;
    if (block) memcpy(block, line, size);
    free(line);
    if (!block) return interpreter->failed ? null_pointer(CT_CHAR) : runtime_error(interpreter, name, interpreter->memory.error);
    return (CtValue){.type = type_pointer(CT_CHAR), .as.address = copy};
}

CtValue bridge_call(CtInterpreter *interpreter, Token name, size_t id, const CtValue *args) {
    Handle *handle = NULL;
    CtInterpreter *child;
    switch (id) {
        case BR_CT_CREATE: return create_engine(interpreter, name);
        case BR_CT_DESTROY:
            if (engine(interpreter, name, args[0], &handle)) discard(interpreter, handle);
            return nothing();
        case BR_CT_CLEAR:
            if ((child = engine(interpreter, name, args[0], NULL))) ct_clear(child);
            return nothing();
        case BR_CT_SET_INTERRUPT:
            if ((child = engine(interpreter, name, args[0], NULL)))
                ct_set_interrupt(child, interpreter->handlers_installed ? &bridge_pending : interpreter->interrupt);
            return nothing();
        case BR_CT_SET_FILENAME: {
            child = engine(interpreter, name, args[0], &handle);
            char *filename = child && address_of(interpreter, name, args[1]) ? builtin_string(interpreter, name, args[1]) : NULL;
            if (!child || interpreter->failed) return nothing();
            char *owned = filename ? strdup(filename) : NULL;
            if (filename && !owned) return runtime_error(interpreter, name, "out of memory");
            ct_set_filename(child, owned);
            free(handle->text);
            handle->text = owned;
            return nothing();
        }
        case BR_CT_SET_LIMITS: {
            child = engine(interpreter, name, args[0], NULL);
            int64_t steps = integer_of(interpreter, name, args[1]), depth = integer_of(interpreter, name, args[2]);
            if (!child || interpreter->failed) return nothing();
            size_t step_budget = steps > 0 ? (size_t)steps : 1000000, frame_budget = depth > 0 ? (unsigned)depth : 256;
            if (step_budget > steps_left(interpreter)) step_budget = steps_left(interpreter);
            if (frame_budget > frames_left(interpreter)) frame_budget = frames_left(interpreter);
            ct_set_limits(child, step_budget, (unsigned)frame_budget);
            return nothing();
        }
        case BR_CT_SET_NESTING: {
            child = engine(interpreter, name, args[0], NULL);
            int64_t level = integer_of(interpreter, name, args[1]), limit = integer_of(interpreter, name, args[2]);
            if (!child || interpreter->failed) return nothing();
            unsigned floor = interpreter->nesting + 1;
            ct_set_nesting(child, level > (int64_t)floor ? (unsigned)level : floor,
                           limit > 0 && (unsigned)limit < interpreter->nesting_limit ? (unsigned)limit : interpreter->nesting_limit);
            return nothing();
        }
        case BR_CT_SET_STRICT:
            if ((child = engine(interpreter, name, args[0], NULL))) ct_set_strict(child, (int)integer_of(interpreter, name, args[1]));
            return nothing();
        case BR_CT_EXIT_STATUS: {
            child = engine(interpreter, name, args[0], NULL);
            uint64_t address = child ? address_of(interpreter, name, args[1]) : 0;
            if (!child || interpreter->failed) return integer(0);
            int status = 0, exiting = ct_exit_status(child, &status);
            return put_int(interpreter, name, address, status) ? integer(exiting) : integer(0);
        }
        case BR_CT_HAS_FUNCTION: {
            child = engine(interpreter, name, args[0], NULL);
            char *function = child ? builtin_string(interpreter, name, args[1]) : NULL;
            return integer(function ? ct_has_function(child, function) : 0);
        }
        case BR_CT_RUN_MAIN: return run_main(interpreter, name, args);
        case BR_CT_INSPECT_TYPE: return inspect_type(interpreter, name, args);
        case BR_CT_DUMP_AST: return parse_only(interpreter, name, args, 3, 1);
        case BR_CT_CHECK: return parse_only(interpreter, name, args, 2, 0);
        case BR_CT_DUMP: {
            child = engine(interpreter, name, args[0], NULL);
            FILE *stream = child ? builtin_stream(interpreter, name, args[1]) : NULL;
            if (stream) ct_dump(child, stream);
            return nothing();
        }
        case BR_CT_SIGNATURE: return lookup_text(interpreter, name, args, 0);
        case BR_CT_COMPLETE: return lookup_text(interpreter, name, args, 1);
        case BR_CT_TYPE_NAME: {
            int64_t type = integer_of(interpreter, name, args[0]), capacity = integer_of(interpreter, name, args[2]);
            uint64_t address = address_of(interpreter, name, args[1]);
            if (interpreter->failed || capacity < 0) return nothing();
            char buffer[256];
            ct_type_name((CtType)type, buffer, sizeof buffer);
            (void)put_text(interpreter, name, address, (size_t)capacity, buffer);
            return nothing();
        }
        case BR_CT_TYPE_SIZE: return (CtValue){.type = CT_ULONG, .as.unsigned_integer = ct_type_size((CtType)integer_of(interpreter, name, args[0]))};
        case BR_CT_EVAL: return evaluate(interpreter, name, args);
        case BR_CT_FORMAT_VALUE: case BR_CT_PRINT_VALUE: {
            int printing = id == BR_CT_PRINT_VALUE;
            child = printing ? engine(interpreter, name, args[0], NULL) : NULL;
            CtValue value;
            const CtValue *rest = args + printing;
            if (!fetch_value(interpreter, name, rest[0], &value)) return nothing();
            uint64_t address = address_of(interpreter, name, rest[1]);
            int64_t capacity = integer_of(interpreter, name, rest[2]);
            if (interpreter->failed || capacity < 0 || (printing && !child)) return nothing();
            char buffer[1024];
            size_t room = (size_t)capacity < sizeof buffer ? (size_t)capacity : sizeof buffer;
            if (printing) ct_print_value(child, value, buffer, room);
            else ct_format_value(value, buffer, room);
            (void)put_text(interpreter, name, address, room, buffer);
            return nothing();
        }
        case BR_CT_DEPTH:
            return (child = engine(interpreter, name, args[0], NULL)) ? (CtValue){.type = CT_UINT, .as.unsigned_integer = ct_depth(child)} : integer(0);
        case BR_BOOT_VERIFY: return check_boot(interpreter, name);
        case BR_TERMINAL_INIT: return terminal_initialize(interpreter, name, args);
        case BR_TERMINAL_READ: return terminal_line(interpreter, name, args);
        case BR_TERMINAL_DESTROY: {
            uint64_t address = address_of(interpreter, name, args[0]);
            handle = interpreter->failed ? NULL : find_handle(interpreter, address, HANDLE_TERMINAL);
            if (handle) discard(interpreter, handle);
            return nothing();
        }
        default: return configuration(interpreter, name, id, args);
    }
}

static void forward(int signal_number) {
    if (signal_number > 0 && signal_number < SIGNAL_SLOTS) {
        signal_arrived[signal_number] = 1;
        bridge_pending = 1;
    }
}

static void settle_pending(void) {
    int any = 0;
    for (int i = 1; i < SIGNAL_SLOTS; ++i) any |= signal_arrived[i];
    bridge_pending = any;
}

int bridge_sigaction(CtInterpreter *interpreter, Token name, const CtValue *args) {
    int64_t number = integer_of(interpreter, name, args[0]);
    uint64_t action_address = address_of(interpreter, name, args[1]);
    uint64_t previous_address = address_of(interpreter, name, args[2]);
    if (interpreter->failed) return -1;
    if (number <= 0 || number >= SIGNAL_SLOTS) return -1;
    int slot = (int)number;
    CtType type = type_is_pointer(args[1].type) ? type_target(args[1].type) : CT_VOID;
    if (previous_address) {
        CtType previous_type = type_is_pointer(args[2].type) ? type_target(args[2].type) : CT_VOID;
        const Member *handler = type_is_aggregate(previous_type) ? type_member(previous_type, "sa_handler", 10) : NULL;
        void *block = window(interpreter, name, previous_address, type_size(previous_type), 1);
        if (!handler || !block) return -1;
        memset(block, 0, type_size(previous_type));
        if (interpreter->signal_handlers[slot].as.address &&
            !memory_write(&interpreter->memory, previous_address + handler->offset, interpreter->signal_handlers[slot]))
            return -1;
    }
    if (!action_address) return 0;
    const Member *handler = type_is_aggregate(type) ? type_member(type, "sa_handler", 10) : NULL;
    if (!handler) { (void)runtime_error(interpreter, name, "sigaction requires a struct sigaction"); return -1; }
    CtValue routine = memory_read(&interpreter->memory, action_address + handler->offset, handler->type);
    if (interpreter->memory.error) { (void)runtime_error(interpreter, name, interpreter->memory.error); return -1; }
    struct sigaction action;
    memset(&action, 0, sizeof action);
    sigemptyset(&action.sa_mask);
    int interpreted = type_is_pointer(routine.type) && routine.as.address && type_is_function(type_target(routine.type));
    action.sa_handler = interpreted ? forward : SIG_DFL;
    struct sigaction *saved = interpreter->signal_installed[slot] ? NULL : malloc(sizeof *saved);
    if (!interpreter->signal_installed[slot] && !saved) { (void)runtime_error(interpreter, name, "out of memory"); return -1; }
    if (sigaction(slot, &action, saved) != 0) { free(saved); return -1; }
    if (saved) {
        interpreter->previous_actions[slot] = saved;
        interpreter->signal_installed[slot] = 1;
    }
    interpreter->signal_handlers[slot] = interpreted ? routine : (CtValue){.type = CT_VOID};
    interpreter->handlers_installed = 0;
    for (int i = 1; i < SIGNAL_SLOTS; ++i) interpreter->handlers_installed |= interpreter->signal_handlers[i].as.address != 0;
    return 0;
}

void bridge_deliver(CtInterpreter *interpreter) {
    CtValue returned = interpreter->returned;
    Node *jump = interpreter->jump;
    for (int i = 1; i < SIGNAL_SLOTS && !interpreter->failed; ++i) {
        if (!signal_arrived[i] || !interpreter->signal_handlers[i].as.address) continue;
        signal_arrived[i] = 0;
        CtValue number = integer(i);
        Token name = {.kind = TK_NAME, .start = "signal", .length = 6, .line = 1, .column = 1};
        (void)runtime_invoke(interpreter, name, interpreter->signal_handlers[i], &number, 1);
    }
    interpreter->returned = returned;
    interpreter->jump = jump;
    settle_pending();
}

void bridge_cleanup(CtInterpreter *interpreter) {
    while (interpreter->handles) discard(interpreter, interpreter->handles);
    for (int i = 1; i < SIGNAL_SLOTS; ++i) {
        if (!interpreter->signal_installed[i]) continue;
        (void)sigaction(i, interpreter->previous_actions[i], NULL);
        free(interpreter->previous_actions[i]);
        interpreter->previous_actions[i] = NULL;
        interpreter->signal_installed[i] = 0;
        interpreter->signal_handlers[i] = (CtValue){.type = CT_VOID};
        signal_arrived[i] = 0;
    }
    interpreter->handlers_installed = 0;
    interpreter->bridge_string_count = 0;
    settle_pending();
}
