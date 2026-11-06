#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE

#include "runtime.h"

#include <ctype.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__x86_64__) && (defined(__linux__) || defined(__APPLE__)) && !defined(CT_NO_JIT)
#define JIT_NATIVE 1
#include <sys/mman.h>
#include <unistd.h>
#else
#define JIT_NATIVE 0
#endif

/* Native code compiles each function on its first call into x86-64 machine
 * code that keeps the interpreter's observable behaviour: every diagnostic is
 * raised by the interpreter's own routines on a slow path, and memory is
 * reached through per-site caches of the interpreter's records, so lifetime,
 * bounds, initialization and read-only checks stay exact. Steps are charged per
 * loop iteration and call rather than per node, which is why CT_JIT_AUTO waits
 * for a step limit large enough that the difference cannot matter. A function
 * using anything the compiler does not know stays interpreted. */

typedef struct JitCode JitCode;
typedef struct JitAccess JitAccess;
struct JitAccess {
    /* The live record the site last reached; native code checks the epoch before trusting it. */
    uint64_t base, size, limit, epoch, writable;
    unsigned char *data, *initialized;
    Node *node;
    CtType pointer, index, element;
    size_t width;
    JitAccess *next;
};

typedef struct JitCall JitCall;
struct JitCall {
    Node *node, *prototype;
    size_t builtin, count;
    CtType result, pointer;
    JitCall *next;
};

enum { JIT_UNTRIED, JIT_COMPILING, JIT_COMPILED, JIT_REJECTED };

struct JitCode {
    Node *function;
    void *entry; /* what native callers jump through: the body once compiled, else the thunk */
    unsigned char *body, *thunk;
    size_t body_size, thunk_size;
    int state;
    JitAccess *accesses;
    JitCall *calls;
    Node **assumed; /* names bound to the library because no global had them */
    size_t assumed_count;
    JitCode *next;
};

struct JitCatch {
    jmp_buf unwind;
    struct JitCatch *previous;
    uintptr_t base, floor;
    unsigned depth;
};

/* Native frames charge the depth limit by stack use rather than per call: a
 * check at entry costs one compare, while counting would chain every call
 * through memory. 256 bytes per unit lets any frame up to that size recurse at
 * least as deep as the interpreter, which charges two or more units a call. */
#define JIT_STACK_PER_DEPTH 256u

static int jit_allowed(const CtInterpreter *interpreter) {
    return JIT_NATIVE && (interpreter->jit_mode == CT_JIT_ON ||
                          (interpreter->jit_mode == CT_JIT_AUTO && interpreter->step_limit >= CT_JIT_AUTO_STEPS));
}

int jit_wanted(const CtInterpreter *interpreter, const Node *function) {
    return jit_allowed(interpreter) && (!function->jit || function->jit->state != JIT_REJECTED);
}

#if !JIT_NATIVE

int jit_invoke(CtInterpreter *interpreter, const Token *name, Node *function,
               const CtValue *values, size_t count, CtValue *result) {
    (void)interpreter; (void)name; (void)function; (void)values; (void)count; (void)result;
    return 0;
}

void jit_reset(CtInterpreter *interpreter) { (void)interpreter; }

void jit_forget(CtInterpreter *interpreter, const Token *name) { (void)interpreter; (void)name; }

#else

static void unmap(void *memory, size_t size) {
    if (memory) (void)munmap(memory, size);
}

static unsigned char *executable(const unsigned char *bytes, size_t length, size_t *mapped) {
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    size_t size = (length + page - 1) / page * page;
    void *memory = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (memory == MAP_FAILED) return NULL;
    memcpy(memory, bytes, length);
    if (mprotect(memory, size, PROT_READ | PROT_EXEC)) {
        unmap(memory, size);
        return NULL;
    }
    *mapped = size;
    return memory;
}

static void free_sites(JitAccess *access, JitCall *call) {
    while (access) {
        JitAccess *next = access->next;
        free(access);
        access = next;
    }
    while (call) {
        JitCall *next = call->next;
        free(call);
        call = next;
    }
}

static void discard_body(JitCode *code) {
    unmap(code->body, code->body_size);
    code->body = NULL;
    code->entry = code->thunk;
    free_sites(code->accesses, code->calls);
    code->accesses = NULL;
    code->calls = NULL;
    free(code->assumed);
    code->assumed = NULL;
    code->assumed_count = 0;
}

void jit_forget(CtInterpreter *interpreter, const Token *name) {
    for (JitCode *code = interpreter->jit_codes; code; code = code->next)
        for (size_t i = 0; i < code->assumed_count; ++i) {
            const Token *assumed = &code->assumed[i]->token;
            if (assumed->length == name->length && !memcmp(assumed->start, name->start, name->length)) {
                discard_body(code);
                code->state = JIT_UNTRIED;
                break;
            }
        }
}

void jit_reset(CtInterpreter *interpreter) {
    while (interpreter->jit_codes) {
        JitCode *code = interpreter->jit_codes;
        interpreter->jit_codes = code->next;
        code->function->jit = NULL;
        discard_body(code);
        unmap(code->thunk, code->thunk_size);
        free(code);
    }
    free(interpreter->jit_objects);
    interpreter->jit_objects = NULL;
    interpreter->jit_object_count = interpreter->jit_object_capacity = 0;
}

static uint64_t bits_of(CtValue value) {
    uint64_t bits = value.as.unsigned_integer;
    if (value.type == CT_DOUBLE) memcpy(&bits, &value.as.real, sizeof bits);
    return bits;
}

static CtValue value_of(CtType type, uint64_t bits) {
    CtValue value = {.type = type};
    if (type == CT_DOUBLE) memcpy(&value.as.real, &bits, sizeof bits);
    else value.as.unsigned_integer = bits;
    return value;
}

static _Noreturn void unwind(CtInterpreter *interpreter) {
    longjmp(interpreter->jit_catch->unwind, 1);
}

static void settle(CtInterpreter *interpreter) {
    if (interpreter->failed || interpreter->exit_requested) unwind(interpreter);
}

/* Charges the native frames below the current entry to the depth the interpreter sees. */
/* The real stack, even under a sanitizer that moves locals elsewhere. */
#define STACK_HERE() ((uintptr_t)__builtin_frame_address(0))

static unsigned enter_interpreter(CtInterpreter *interpreter) {
    unsigned depth = interpreter->depth;
    uintptr_t used = interpreter->jit_catch->base - STACK_HERE();
    interpreter->depth = interpreter->jit_catch->depth + (unsigned)(used / JIT_STACK_PER_DEPTH) + 1;
    return depth;
}

enum {
    FAIL_DIVISION, FAIL_OVERFLOW, FAIL_SHIFT, FAIL_NEGATIVE_SHIFT, FAIL_NOT_FINITE, FAIL_RANGE,
    FAIL_POINTER_NUMBER, FAIL_POINTER_ZERO, FAIL_UNINITIALIZED, FAIL_DEPTH, FAIL_NO_RETURN, FAIL_CONST
};

static const char *const failures[] = {
    [FAIL_DIVISION] = "division by zero",
    [FAIL_OVERFLOW] = "signed integer overflow",
    [FAIL_SHIFT] = "shift count is outside the width of the type",
    [FAIL_NEGATIVE_SHIFT] = "left shift of a negative value",
    [FAIL_NOT_FINITE] = "floating-point result is not finite",
    [FAIL_RANGE] = "conversion to an integer type is out of range",
    [FAIL_POINTER_NUMBER] = "cannot convert a pointer to a number",
    [FAIL_POINTER_ZERO] = "pointer conversion requires a pointer or zero",
    [FAIL_UNINITIALIZED] = "read of uninitialized memory",
    [FAIL_DEPTH] = "evaluation nesting limit exceeded",
    [FAIL_NO_RETURN] = "function finished without returning a value",
    [FAIL_CONST] = "assignment to a const object",
};

static void jit_fail(CtInterpreter *interpreter, Node *node, int failure) {
    if (failure == FAIL_UNINITIALIZED) {
        interpreter->memory.error = failures[failure];
        (void)runtime_memory_error(interpreter, &node->token);
    } else (void)runtime_error(interpreter, node->token, failures[failure]);
    unwind(interpreter);
}

static void jit_tick(CtInterpreter *interpreter, Node *node) {
    settle(interpreter);
    unsigned depth = enter_interpreter(interpreter);
    if (interpreter->steps > interpreter->step_limit) {
        (void)runtime_error(interpreter, node->token, "execution step limit exceeded");
        unwind(interpreter);
    }
    if (interpreter->interrupt && *interpreter->interrupt) {
        (void)runtime_error(interpreter, node->token, "execution interrupted");
        unwind(interpreter);
    }
    if (bridge_pending) {
        bridge_deliver(interpreter);
        settle(interpreter);
    }
    interpreter->depth = depth;
    runtime_rearm(interpreter);
}

static void remember(CtInterpreter *interpreter, JitAccess *access, uint64_t address) {
    Allocation *record = memory_find(&interpreter->memory, address);
    if (!record || !record->alive || !record->address || record->size < access->width) return;
    access->base = record->address;
    access->size = record->size;
    access->limit = record->size - access->width;
    access->data = record->data;
    access->initialized = record->fully_initialized ? NULL : record->initialized;
    access->writable = !record->readonly;
    access->epoch = interpreter->memory.epoch;
}

static uint64_t jit_element(CtInterpreter *interpreter, JitAccess *access, uint64_t pointer, uint64_t index) {
    CtValue at = runtime_binary(interpreter, '+', &access->node->token, value_of(access->pointer, pointer),
                                value_of(access->index, index));
    settle(interpreter);
    remember(interpreter, access, pointer);
    return at.as.address;
}

static uint64_t jit_load(CtInterpreter *interpreter, JitAccess *access, uint64_t address) {
    CtValue value = runtime_load(interpreter, &access->node->token, address, access->element);
    settle(interpreter);
    remember(interpreter, access, address);
    return bits_of(value);
}

#define VIEW_FAR (UINT64_C(1) << 62)

static void jit_view(CtInterpreter *interpreter, JitAccess *site, uint64_t pointer, uint64_t *slots) {
    remember(interpreter, site, pointer);
    Allocation *record = memory_find(&interpreter->memory, pointer);
    int valid = record && record->alive && record->address &&
                pointer - record->address <= record->size && record->size >= site->width &&
                !(site->width > 1 && (pointer & (site->width - 1)));
    if (!valid) {
        slots[0] = slots[1] = VIEW_FAR;
        slots[2] = slots[3] = slots[4] = 0;
        return;
    }
    slots[0] = pointer - record->address;
    slots[1] = record->readonly ? VIEW_FAR : slots[0];
    slots[2] = record->size - site->width;
    slots[3] = (uint64_t)(uintptr_t)record->data;
    slots[4] = record->fully_initialized ? 0 : (uint64_t)(uintptr_t)record->initialized;
}

static uint64_t jit_load_element(CtInterpreter *interpreter, JitAccess *access, uint64_t pointer, uint64_t index) {
    uint64_t address = jit_element(interpreter, access, pointer, index);
    return access->width ? jit_load(interpreter, access, address) : address;
}

static void jit_store(CtInterpreter *interpreter, JitAccess *access, uint64_t address, uint64_t bits) {
    (void)runtime_store(interpreter, &access->node->token, address, access->element, value_of(access->element, bits));
    settle(interpreter);
    remember(interpreter, access, address);
}

static uint64_t jit_pointer_offset(CtInterpreter *interpreter, JitAccess *access, uint64_t pointer, uint64_t count, int op) {
    CtValue result = runtime_binary(interpreter, op, &access->node->token, value_of(access->pointer, pointer),
                                    value_of(access->index, count));
    settle(interpreter);
    remember(interpreter, access, pointer);
    return result.as.address;
}

/* Everything else pointers can do, with operand types left in the site's pointer and index. */
static uint64_t jit_pointer_binary(CtInterpreter *interpreter, JitAccess *access, uint64_t left, uint64_t right, int op) {
    CtValue result = runtime_binary(interpreter, op, &access->node->token, value_of(access->pointer, left),
                                    value_of(access->index, right));
    settle(interpreter);
    return bits_of(runtime_convert(interpreter, access->node->token, result, access->element));
}

static uint64_t jit_call_library(CtInterpreter *interpreter, JitCall *call, const CtValue *values) {
    unsigned depth = enter_interpreter(interpreter);
    CtValue result = runtime_call_builtin(interpreter, &call->node->left->token, call->builtin, call->prototype,
                                          values, call->count);
    settle(interpreter);
    interpreter->depth = depth;
    if (call->result == CT_VOID) return 0;
    result = runtime_convert(interpreter, call->node->token, result, call->result);
    settle(interpreter);
    return bits_of(result);
}

static uint64_t jit_call_pointer(CtInterpreter *interpreter, JitCall *call, uint64_t pointer, const CtValue *values) {
    Node *callee = call->node->left;
    const Token *name = callee->kind == N_NAME ? &callee->token : &call->node->token;
    unsigned depth = enter_interpreter(interpreter);
    CtValue result = runtime_call_address(interpreter, name, value_of(call->pointer, pointer), values, call->count);
    settle(interpreter);
    interpreter->depth = depth;
    if (call->result == CT_VOID) return 0;
    result = runtime_convert(interpreter, *name, result, call->result);
    settle(interpreter);
    return bits_of(result);
}

static void jit_copy(CtInterpreter *interpreter, Node *node, uint64_t destination, uint64_t source, CtType type) {
    (void)runtime_store(interpreter, &node->token, destination, type, (CtValue){.type = type, .as.address = source});
    settle(interpreter);
}

static void jit_initialize(CtInterpreter *interpreter, Node *declaration, uint64_t address) {
    unsigned depth = enter_interpreter(interpreter);
    runtime_initialize(interpreter, address, declaration);
    settle(interpreter);
    interpreter->depth = depth;
}

static uint64_t jit_call_interpreted(CtInterpreter *interpreter, Node *function, const CtValue *values, size_t count) {
    unsigned depth = enter_interpreter(interpreter);
    CtValue result = runtime_call_function(interpreter, &function->token, function, values, count);
    settle(interpreter);
    interpreter->depth = depth;
    return type_target(function->type) == CT_VOID ? 0 : bits_of(result);
}

static uint64_t jit_library_value(CtInterpreter *interpreter, Node *name) {
    CtValue value = {0};
    if (!builtin_value(interpreter, name->token, &value)) (void)runtime_error(interpreter, name->token, "unknown name");
    settle(interpreter);
    return value.as.address;
}

static uint64_t jit_string(CtInterpreter *interpreter, Node *node) {
    if (!node->address) {
        node->address = memory_allocate(&interpreter->memory, node->text_length + 1, 1, 0);
        if (!node->address) {
            (void)runtime_memory_error(interpreter, &node->token);
            unwind(interpreter);
        }
        memcpy(memory_access(&interpreter->memory, node->address, node->text_length + 1, 1), node->text, node->text_length + 1);
        memory_find(&interpreter->memory, node->address)->readonly = 1;
    }
    return node->address;
}

static uint64_t jit_object(CtInterpreter *interpreter, Node *declaration) {
    Allocation *record = memory_allocate_record(&interpreter->memory, ct_type_size(declaration->type),
                                                declaration->left != NULL, 0);
    if (!record) {
        (void)runtime_memory_error(interpreter, &declaration->token);
        unwind(interpreter);
    }
    if (interpreter->jit_object_count == interpreter->jit_object_capacity) {
        size_t capacity = interpreter->jit_object_capacity ? 2 * interpreter->jit_object_capacity : 32;
        uint64_t *grown = realloc(interpreter->jit_objects, capacity * sizeof *grown);
        if (!grown) {
            memory_release_record(&interpreter->memory, record);
            (void)runtime_error(interpreter, declaration->token, "out of memory");
            unwind(interpreter);
        }
        interpreter->jit_objects = grown;
        interpreter->jit_object_capacity = capacity;
    }
    interpreter->jit_objects[interpreter->jit_object_count++] = record->address;
    return record->address;
}

static void jit_protect(CtInterpreter *interpreter, uint64_t address) {
    memory_find(&interpreter->memory, address)->readonly = 1;
    ++interpreter->memory.epoch;
}

static void release_objects(CtInterpreter *interpreter, size_t keep) {
    while (interpreter->jit_object_count > keep)
        (void)memory_release(&interpreter->memory, interpreter->jit_objects[--interpreter->jit_object_count], 0);
}

static void jit_release(CtInterpreter *interpreter, uint64_t address) {
    if (!address) return;
    for (size_t i = interpreter->jit_object_count; i-- > 0;)
        if (interpreter->jit_objects[i] == address) {
            memmove(interpreter->jit_objects + i, interpreter->jit_objects + i + 1,
                    (interpreter->jit_object_count - i - 1) * sizeof *interpreter->jit_objects);
            --interpreter->jit_object_count;
            (void)memory_release(&interpreter->memory, address, 0);
            return;
        }
}

static uint64_t jit_static(CtInterpreter *interpreter, Node *declaration) {
    if (declaration->address) return declaration->address;
    Allocation *record = memory_allocate_record(&interpreter->memory, ct_type_size(declaration->type), 1, 0);
    if (!record) {
        (void)runtime_memory_error(interpreter, &declaration->token);
        unwind(interpreter);
    }
    declaration->address = record->address;
    unsigned depth = enter_interpreter(interpreter);
    if (declaration->left && declaration->left->kind == N_VALUE && type_is_scalar(declaration->type))
        (void)runtime_store(interpreter, &declaration->left->token, record->address, declaration->type,
                            declaration->left->token.value);
    else if (declaration->left) runtime_initialize(interpreter, record->address, declaration);
    if (declaration->is_const) record->readonly = 1;
    settle(interpreter);
    interpreter->depth = depth;
    return record->address;
}

enum { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15, RIP };
enum { CC_O, CC_NO, CC_B, CC_AE, CC_E, CC_NE, CC_BE, CC_A, CC_S, CC_NS, CC_P, CC_NP, CC_L, CC_GE, CC_LE, CC_G };
enum { ALU_ADD = 0, ALU_OR = 1, ALU_AND = 4, ALU_SUB = 5, ALU_XOR = 6, ALU_CMP = 7 };
enum { HOT, COLD, DATA, SECTIONS };

typedef struct { unsigned char *bytes; size_t length, capacity; } Section;
typedef struct { int section; size_t offset; } Place;
typedef struct { int section; size_t at; int label, table; } Fixup;

enum { LOCAL_SLOT, LOCAL_MANAGED, LOCAL_STATIC };

typedef struct {
    Node *declaration;
    CtType type;
    int storage, offset, reg, flag, incoming, addressed, readonly;
    double weight;
} Local;

typedef struct { int break_label, continue_label; size_t live; } Loop;

/* An array, or a pointer a loop never changes, validated once on entering a
 * loop that calls nothing and so cannot end any lifetime. Its five slots hold
 * the pointer's offset in its object for loads and for stores, the last offset
 * an element may start at, the object's host address, and its initialization
 * flags while it has any uninitialized byte. */
typedef struct {
    Local *local;
    Symbol *symbol;
    CtType element;
    size_t width;
    int slots;
    JitAccess *site;
} View;

#define MAX_VIEWS 16
#define VIEW_SLOT_BYTES 40

typedef struct {
    CtInterpreter *interpreter;
    JitCode *code;
    Node *function;
    Section sections[SECTIONS];
    int section;
    Place *labels;
    size_t label_count, label_capacity;
    Fixup *fixups;
    size_t fixup_count, fixup_capacity;
    struct { uint64_t bits; int label; } constants[64];
    size_t constant_count;
    Local *locals;
    size_t local_count, local_capacity;
    size_t *live;
    size_t live_count, live_capacity;
    Loop loops[64];
    size_t loop_count;
    View views[MAX_VIEWS];
    size_t view_count, view_groups, view_groups_used;
    int view_base;
    int saved[3], saved_count;
    Local *xmm_locals[6];
    int xmm_saves[6], xmm_count;
    int local_bytes, temp_depth, temp_peak, outgoing_peak;
    size_t frame_patch;
    int return_label, entry_label;
    CtType returns;
    JitAccess *accesses;
    JitCall *calls;
    Node **assumed;
    size_t assumed_count, assumed_capacity;
    int right_operand; /* where apply() finds its right operand: rcx unless a register local stands in */
    int discarding;    /* the increment being compiled has no use for its old value */
    const char *reason;
    Node *reason_node;
    int failed;
} Compiler;

static int reject(Compiler *c, Node *node, const char *reason) {
    if (!c->failed) {
        c->failed = 1;
        c->reason = reason;
        c->reason_node = node;
    }
    return 0;
}

static void emit(Compiler *c, unsigned value) {
    Section *section = &c->sections[c->section];
    if (section->length == section->capacity) {
        size_t capacity = section->capacity ? 2 * section->capacity : 1024;
        unsigned char *grown = realloc(section->bytes, capacity);
        if (!grown) {
            (void)reject(c, c->function, "out of memory");
            section->length = 0;
            return;
        }
        section->bytes = grown;
        section->capacity = capacity;
    }
    section->bytes[section->length++] = (unsigned char)value;
}

static void emit32(Compiler *c, uint32_t value) {
    for (int i = 0; i < 4; ++i) emit(c, (value >> (8 * i)) & 0xFF);
}

static void emit64(Compiler *c, uint64_t value) {
    for (int i = 0; i < 8; ++i) emit(c, (unsigned)(value >> (8 * i)) & 0xFF);
}

static int new_label(Compiler *c) {
    if (c->label_count == c->label_capacity) {
        size_t capacity = c->label_capacity ? 2 * c->label_capacity : 256;
        Place *grown = realloc(c->labels, capacity * sizeof *grown);
        if (!grown) return reject(c, c->function, "out of memory");
        c->labels = grown;
        c->label_capacity = capacity;
    }
    c->labels[c->label_count] = (Place){-1, 0};
    return (int)c->label_count++;
}

static void bind(Compiler *c, int label) {
    c->labels[label] = (Place){c->section, c->sections[c->section].length};
}

static void fixup(Compiler *c, int label, int table) {
    if (c->fixup_count == c->fixup_capacity) {
        size_t capacity = c->fixup_capacity ? 2 * c->fixup_capacity : 256;
        Fixup *grown = realloc(c->fixups, capacity * sizeof *grown);
        if (!grown) { (void)reject(c, c->function, "out of memory"); return; }
        c->fixups = grown;
        c->fixup_capacity = capacity;
    }
    c->fixups[c->fixup_count++] = (Fixup){c->section, c->sections[c->section].length, label, table};
    emit32(c, 0);
}

static int constant(Compiler *c, uint64_t bits) {
    for (size_t i = 0; i < c->constant_count; ++i)
        if (c->constants[i].bits == bits) return c->constants[i].label;
    if (c->constant_count == sizeof c->constants / sizeof c->constants[0]) return reject(c, c->function, "too many constants");
    int saved = c->section;
    c->section = DATA;
    while (c->sections[DATA].length % 8) emit(c, 0);
    int label = new_label(c);
    bind(c, label);
    emit64(c, bits);
    c->section = saved;
    c->constants[c->constant_count].bits = bits;
    c->constants[c->constant_count++].label = label;
    return label;
}

static int real_constant(Compiler *c, double value) {
    uint64_t bits;
    memcpy(&bits, &value, sizeof bits);
    return constant(c, bits);
}

static void prefix_rex(Compiler *c, int prefix, int w, int reg, int index, int base, int force) {
    if (prefix) emit(c, (unsigned)prefix);
    unsigned rex = 0x40u | (w ? 8u : 0u) | ((unsigned)(reg >> 3) & 1u) << 2 | ((unsigned)(index >> 3) & 1u) << 1 |
                   (base == RIP ? 0u : (unsigned)(base >> 3) & 1u);
    if (rex != 0x40 || force) emit(c, rex);
}

static void opcode(Compiler *c, unsigned op) {
    if (op > 0xFF) emit(c, op >> 8);
    emit(c, op & 0xFF);
}

static void op_rr(Compiler *c, int prefix, int w, unsigned op, int reg, int rm, int force) {
    prefix_rex(c, prefix, w, reg, 0, rm, force);
    opcode(c, op);
    emit(c, 0xC0u | (unsigned)(reg & 7) << 3 | (unsigned)(rm & 7));
}

/* With base RIP, disp names the label of a constant. */
static void op_rm(Compiler *c, int prefix, int w, unsigned op, int reg, int base, int32_t disp, int force) {
    prefix_rex(c, prefix, w, reg, 0, base, force);
    opcode(c, op);
    if (base == RIP) {
        emit(c, 0x05u | (unsigned)(reg & 7) << 3);
        fixup(c, disp, 0);
        return;
    }
    unsigned mod = disp == 0 && (base & 7) != RBP ? 0u : disp >= -128 && disp <= 127 ? 1u : 2u;
    emit(c, mod << 6 | (unsigned)(reg & 7) << 3 | (unsigned)(base & 7));
    if ((base & 7) == RSP) emit(c, 0x24);
    if (mod == 1) emit(c, (unsigned)disp & 0xFF);
    else if (mod == 2) emit32(c, (uint32_t)disp);
}

static void op_sib(Compiler *c, int w, unsigned op, int reg, int base, int index, int scale) {
    prefix_rex(c, 0, w, reg, index, base, 0);
    opcode(c, op);
    unsigned sib = (unsigned)scale << 6 | (unsigned)(index & 7) << 3 | (unsigned)(base & 7);
    if ((base & 7) == RBP) {
        emit(c, 0x44u | (unsigned)(reg & 7) << 3);
        emit(c, sib);
        emit(c, 0);
    } else {
        emit(c, 0x04u | (unsigned)(reg & 7) << 3);
        emit(c, sib);
    }
}

static void mov_rr(Compiler *c, int dst, int src) { if (dst != src) op_rr(c, 0, 1, 0x89, src, dst, 0); }
static void mov_rr32(Compiler *c, int dst, int src) { op_rr(c, 0, 0, 0x89, src, dst, 0); }
static void load64(Compiler *c, int dst, int base, int32_t disp) { op_rm(c, 0, 1, 0x8B, dst, base, disp, 0); }
static void store64(Compiler *c, int base, int32_t disp, int src) { op_rm(c, 0, 1, 0x89, src, base, disp, 0); }
static void lea(Compiler *c, int dst, int base, int32_t disp) { op_rm(c, 0, 1, 0x8D, dst, base, disp, 0); }

static void mov_imm(Compiler *c, int dst, uint64_t value) {
    if (value <= 0xFFFFFFFFu) {
        prefix_rex(c, 0, 0, 0, 0, dst, 0);
        emit(c, 0xB8u + (unsigned)(dst & 7));
        emit32(c, (uint32_t)value);
    } else if ((int64_t)value >= INT32_MIN && (int64_t)value < 0) {
        op_rr(c, 0, 1, 0xC7, 0, dst, 0);
        emit32(c, (uint32_t)value);
    } else {
        prefix_rex(c, 0, 1, 0, 0, dst, 0);
        emit(c, 0xB8u + (unsigned)(dst & 7));
        emit64(c, value);
    }
}

static void mov_pointer(Compiler *c, int dst, const void *pointer) { mov_imm(c, dst, (uint64_t)(uintptr_t)pointer); }

static void alu_rr(Compiler *c, int op, int w, int dst, int src) { op_rr(c, 0, w, (unsigned)op * 8 + 1, src, dst, 0); }
static void alu_rm(Compiler *c, int op, int w, int dst, int base, int32_t disp) {
    op_rm(c, 0, w, (unsigned)op * 8 + 3, dst, base, disp, 0);
}

static void alu_ri(Compiler *c, int op, int w, int dst, int32_t value) {
    if (value >= -128 && value <= 127) {
        op_rr(c, 0, w, 0x83, op, dst, 0);
        emit(c, (unsigned)value & 0xFF);
    } else {
        op_rr(c, 0, w, 0x81, op, dst, 0);
        emit32(c, (uint32_t)value);
    }
}

static void alu_mi(Compiler *c, int op, int w, int base, int32_t disp, int32_t value) {
    if (value >= -128 && value <= 127) {
        op_rm(c, 0, w, 0x83, op, base, disp, 0);
        emit(c, (unsigned)value & 0xFF);
    } else {
        op_rm(c, 0, w, 0x81, op, base, disp, 0);
        emit32(c, (uint32_t)value);
    }
}

static int fits32(int64_t value) { return value >= INT32_MIN && value <= INT32_MAX; }

static void test_rr(Compiler *c, int w, int a, int b) { op_rr(c, 0, w, 0x85, b, a, 0); }
static void group3(Compiler *c, int extension, int w, int reg) { op_rr(c, 0, w, 0xF7, extension, reg, 0); }
static void shift_cl(Compiler *c, int extension, int w, int reg) { op_rr(c, 0, w, 0xD3, extension, reg, 0); }
static void imul_rr(Compiler *c, int w, int dst, int src) { op_rr(c, 0, w, 0x0FAF, dst, src, 0); }

static void imul_ri(Compiler *c, int w, int dst, int src, int32_t value) {
    op_rr(c, 0, w, 0x69, dst, src, 0);
    emit32(c, (uint32_t)value);
}

static void setcc(Compiler *c, int cc, int reg) {
    op_rr(c, 0, 0, 0x0F90u + (unsigned)cc, 0, reg, 1);
    op_rr(c, 0, 0, 0x0FB6, reg, reg, 1);
}

static void jcc(Compiler *c, int cc, int label) {
    emit(c, 0x0F);
    emit(c, 0x80u + (unsigned)cc);
    fixup(c, label, 0);
}

static void jmp(Compiler *c, int label) {
    emit(c, 0xE9);
    fixup(c, label, 0);
}

typedef void (*Helper)(void);

static void movsd_load(Compiler *c, int xmm, int base, int32_t disp);
static void movsd_store(Compiler *c, int base, int32_t disp, int xmm);

/* C clobbers every xmm register, so doubles kept in xmm8-13 wait in their homes meanwhile. */
static void call_helper(Compiler *c, Helper helper) {
    uint64_t address;
    memcpy(&address, &helper, sizeof address);
    for (int i = 0; i < c->xmm_count; ++i) movsd_store(c, RBP, c->xmm_locals[i]->offset, c->xmm_locals[i]->reg);
    mov_imm(c, RAX, address);
    op_rr(c, 0, 0, 0xFF, 2, RAX, 0);
    for (int i = 0; i < c->xmm_count; ++i) movsd_load(c, c->xmm_locals[i]->reg, RBP, c->xmm_locals[i]->offset);
}

#define INTERPRETER(field) ((int32_t)offsetof(CtInterpreter, field))

/* r14 holds the steps left before the next slow check, so that native code
 * counts in a register. Code that may run the interpreter needs the count in
 * memory first, and picks up whatever the interpreter left there after. */
enum { BUDGET = R14 };

static void store_steps(Compiler *c) {
    load64(c, R11, R15, INTERPRETER(tick_boundary));
    alu_rr(c, ALU_SUB, 1, R11, BUDGET);
    store64(c, R15, INTERPRETER(steps), R11);
}

static void load_budget(Compiler *c) {
    load64(c, BUDGET, R15, INTERPRETER(tick_boundary));
    alu_rm(c, ALU_SUB, 1, BUDGET, R15, INTERPRETER(steps));
}

static void call_interpreter(Compiler *c, Helper helper) {
    store_steps(c);
    call_helper(c, helper);
    load_budget(c);
}

static void push(Compiler *c, int reg) {
    if (reg >= 8) emit(c, 0x41);
    emit(c, 0x50u + (unsigned)(reg & 7));
}

static void pop(Compiler *c, int reg) {
    if (reg >= 8) emit(c, 0x41);
    emit(c, 0x58u + (unsigned)(reg & 7));
}

static void sse_rr(Compiler *c, int prefix, unsigned op, int reg, int rm) { op_rr(c, prefix, 0, 0x0F00u | op, reg, rm, 0); }
static void sse_rm(Compiler *c, int prefix, unsigned op, int reg, int base, int32_t disp) {
    op_rm(c, prefix, 0, 0x0F00u | op, reg, base, disp, 0);
}

static void movsd_load(Compiler *c, int xmm, int base, int32_t disp) { sse_rm(c, 0xF2, 0x10, xmm, base, disp); }
static void movsd_store(Compiler *c, int base, int32_t disp, int xmm) { sse_rm(c, 0xF2, 0x11, xmm, base, disp); }
static void movapd(Compiler *c, int dst, int src) { if (dst != src) sse_rr(c, 0x66, 0x28, dst, src); }
static void movq_to_xmm(Compiler *c, int xmm, int gpr) { op_rr(c, 0x66, 1, 0x0F6E, xmm, gpr, 0); }
static void movq_from_xmm(Compiler *c, int gpr, int xmm) { op_rr(c, 0x66, 1, 0x0F7E, xmm, gpr, 0); }
static void ucomisd(Compiler *c, int a, int b) { sse_rr(c, 0x66, 0x2E, a, b); }
static void cvtsi2sd(Compiler *c, int xmm, int gpr) { op_rr(c, 0xF2, 1, 0x0F2A, xmm, gpr, 0); }
static void cvttsd2si(Compiler *c, int gpr, int xmm) { op_rr(c, 0xF2, 1, 0x0F2C, gpr, xmm, 0); }

static void load_real_constant(Compiler *c, int xmm, double value) {
    uint64_t bits;
    memcpy(&bits, &value, sizeof bits);
    if (!bits) sse_rr(c, 0x66, 0x57, xmm, xmm);
    else movsd_load(c, xmm, RIP, real_constant(c, value));
}

static int is_real(CtType type) { return type == CT_DOUBLE; }

static int supported(CtType type) {
    TypeKind kind = type_kind(type);
    return kind <= TY_ULLONG || kind == TY_DOUBLE || kind == TY_POINTER;
}

static int wide(CtType type) { return type_size(type) == 8 || type_kind(type) == TY_POINTER; }

/* Integers live in registers sign- or zero-extended to 64 bits from their own width. */
static void canonical(Compiler *c, CtType type, int reg) {
    switch (type_kind(type)) {
        case TY_BOOL:
            test_rr(c, 1, reg, reg);
            setcc(c, CC_NE, reg);
            break;
        case TY_CHAR: case TY_SCHAR: op_rr(c, 0, 1, 0x0FBE, reg, reg, 1); break;
        case TY_UCHAR: op_rr(c, 0, 0, 0x0FB6, reg, reg, 1); break;
        case TY_SHORT: op_rr(c, 0, 1, 0x0FBF, reg, reg, 0); break;
        case TY_USHORT: op_rr(c, 0, 0, 0x0FB7, reg, reg, 0); break;
        case TY_INT: op_rr(c, 0, 1, 0x63, reg, reg, 0); break;
        case TY_UINT: mov_rr32(c, reg, reg); break;
        default: break;
    }
}

static int integer_fits(CtType from, CtType to) {
    if (from == to || type_kind(from) == TY_BOOL) return 1;
    int from_signed = type_is_signed(from), to_signed = type_is_signed(to);
    size_t from_size = type_size(from), to_size = type_size(to);
    if (from_signed == to_signed) return from_size <= to_size;
    return !from_signed && from_size < to_size;
}

static int cold_label(Compiler *c, Node *node, int failure);

static void real_to_integer(Compiler *c, CtType to, Node *node, int gpr, int xmm) {
    int bad = cold_label(c, node, FAIL_RANGE);
    int is_signed = type_is_signed(to);
    size_t size = type_size(to);
    if (size == 8 && !is_signed) {
        int large = new_label(c), done = new_label(c);
        load_real_constant(c, 15, -1.0);
        ucomisd(c, xmm, 15);
        jcc(c, CC_BE, bad);
        load_real_constant(c, 15, 18446744073709551616.0);
        ucomisd(c, xmm, 15);
        jcc(c, CC_AE, bad);
        load_real_constant(c, 15, 9223372036854775808.0);
        ucomisd(c, xmm, 15);
        jcc(c, CC_AE, large);
        cvttsd2si(c, gpr, xmm);
        jmp(c, done);
        bind(c, large);
        movapd(c, 14, xmm);
        sse_rr(c, 0xF2, 0x5C, 14, 15);
        cvttsd2si(c, gpr, 14);
        mov_imm(c, R11, UINT64_C(1) << 63);
        alu_rr(c, ALU_XOR, 1, gpr, R11);
        bind(c, done);
        return;
    }
    double low, high;
    if (size == 8) {
        low = -9223372036854775808.0;
        high = 9223372036854775808.0;
        load_real_constant(c, 15, low);
        ucomisd(c, xmm, 15);
        jcc(c, CC_B, bad);
    } else {
        low = is_signed ? (double)type_minimum(to) - 1.0 : -1.0;
        high = is_signed ? (double)type_maximum(to) + 1.0 : (double)type_mask(to) + 1.0;
        load_real_constant(c, 15, low);
        ucomisd(c, xmm, 15);
        jcc(c, CC_BE, bad);
    }
    load_real_constant(c, 15, high);
    ucomisd(c, xmm, 15);
    jcc(c, CC_AE, bad);
    cvttsd2si(c, gpr, xmm);
}

static void integer_to_real(Compiler *c, CtType from, int gpr, int xmm) {
    if (!(wide(from) && !type_is_signed(from))) {
        cvtsi2sd(c, xmm, gpr);
        return;
    }
    int large = new_label(c), done = new_label(c);
    test_rr(c, 1, gpr, gpr);
    jcc(c, CC_S, large);
    cvtsi2sd(c, xmm, gpr);
    jmp(c, done);
    bind(c, large);
    mov_rr(c, R11, gpr);
    op_rr(c, 0, 1, 0xD1, 5, R11, 0);
    mov_rr(c, R9, gpr);
    alu_ri(c, ALU_AND, 1, R9, 1);
    alu_rr(c, ALU_OR, 1, R11, R9);
    cvtsi2sd(c, xmm, R11);
    sse_rr(c, 0xF2, 0x58, xmm, xmm);
    bind(c, done);
}

/* The interpreter's convert(): an integer lives in gpr, a double in xmm. */
static void convert(Compiler *c, CtType from, CtType to, int explicit_cast, Node *node, int gpr, int xmm) {
    if (from == to || to == CT_VOID || c->failed) return;
    if (from == CT_VOID) { (void)reject(c, node, "void value used in an expression"); return; }
    TypeKind target = type_kind(to), source = type_kind(from);
    if (!supported(to) || !supported(from)) { (void)reject(c, node, "unsupported conversion"); return; }
    if (source == TY_POINTER && target != TY_POINTER && (!explicit_cast || target == TY_DOUBLE)) {
        jmp(c, cold_label(c, node, FAIL_POINTER_NUMBER));
        return;
    }
    if (target == TY_BOOL) {
        if (source == TY_DOUBLE) {
            sse_rr(c, 0x66, 0x57, 15, 15);
            ucomisd(c, xmm, 15);
            setcc(c, CC_NE, gpr);
        } else canonical(c, to, gpr);
        return;
    }
    if (target <= TY_ULLONG) {
        if (source == TY_DOUBLE) real_to_integer(c, to, node, gpr, xmm);
        else if (source == TY_POINTER || !integer_fits(from, to)) canonical(c, to, gpr);
        return;
    }
    if (target == TY_DOUBLE) {
        integer_to_real(c, from, gpr, xmm);
        return;
    }
    if (source == TY_POINTER) return;
    if (source == TY_DOUBLE) { jmp(c, cold_label(c, node, FAIL_POINTER_ZERO)); return; }
    if (!explicit_cast) {
        test_rr(c, 1, gpr, gpr);
        jcc(c, CC_NE, cold_label(c, node, FAIL_POINTER_ZERO));
    }
}

static void load_typed(Compiler *c, CtType type, int dst, int base, int32_t disp) {
    if (is_real(type)) { movsd_load(c, dst, base, disp); return; }
    int is_signed = type_kind(type) != TY_POINTER && type_is_signed(type);
    switch (type_size(type)) {
        case 1: op_rm(c, 0, is_signed, is_signed ? 0x0FBE : 0x0FB6, dst, base, disp, 0); break;
        case 2: op_rm(c, 0, is_signed, is_signed ? 0x0FBF : 0x0FB7, dst, base, disp, 0); break;
        case 4: op_rm(c, 0, is_signed, is_signed ? 0x63 : 0x8B, dst, base, disp, 0); break;
        default: load64(c, dst, base, disp); break;
    }
}

static void store_typed(Compiler *c, CtType type, int base, int32_t disp, int src) {
    if (is_real(type)) { movsd_store(c, base, disp, src); return; }
    switch (type_size(type)) {
        case 1: op_rm(c, 0, 0, 0x88, src, base, disp, 1); break;
        case 2: op_rm(c, 0x66, 0, 0x89, src, base, disp, 0); break;
        case 4: op_rm(c, 0, 0, 0x89, src, base, disp, 0); break;
        default: store64(c, base, disp, src); break;
    }
}

/* Out-of-line code goes to the cold section. Code already there jumps over
 * it, so that its fast path never falls into a stub. The returned token
 * restores the section and binds that jump's target. */
static int enter_cold(Compiler *c) {
    int skip = -1;
    if (c->section == COLD) {
        skip = new_label(c);
        jmp(c, skip);
    }
    int saved = c->section + SECTIONS * (skip + 1);
    c->section = COLD;
    return saved;
}

static void leave_cold(Compiler *c, int saved) {
    int skip = saved / SECTIONS - 1;
    if (skip >= 0) bind(c, skip);
    c->section = saved % SECTIONS;
}

static void open_cold(Compiler *c, int label, int *saved) {
    *saved = enter_cold(c);
    bind(c, label);
}

static void close_cold(Compiler *c, int back, int saved) {
    jmp(c, back);
    leave_cold(c, saved);
}

static int cold_label(Compiler *c, Node *node, int failure) {
    int label = new_label(c), saved = enter_cold(c);
    bind(c, label);
    mov_rr(c, RDI, R15);
    mov_pointer(c, RSI, node);
    mov_imm(c, RDX, (uint64_t)failure);
    call_helper(c, (Helper)jit_fail);
    leave_cold(c, saved);
    return label;
}

#define EPOCH ((int32_t)(offsetof(CtInterpreter, memory) + offsetof(Memory, epoch)))
#define SITE(field) ((int32_t)offsetof(JitAccess, field))

static void tick(Compiler *c, Node *node, size_t weight) {
    int slow = new_label(c), back = new_label(c);
    alu_ri(c, ALU_SUB, 1, BUDGET, (int32_t)(weight > 1000000 ? 1000000 : weight));
    jcc(c, CC_S, slow);
    bind(c, back);
    int saved;
    open_cold(c, slow, &saved);
    mov_rr(c, RDI, R15);
    mov_pointer(c, RSI, node);
    call_interpreter(c, (Helper)jit_tick);
    close_cold(c, back, saved);
}

static JitAccess *new_access(Compiler *c, Node *node, CtType pointer, CtType index, CtType element, size_t width) {
    JitAccess *access = calloc(1, sizeof *access);
    if (!access) { (void)reject(c, node, "out of memory"); return NULL; }
    *access = (JitAccess){.epoch = UINT64_MAX, .node = node, .pointer = pointer, .index = index,
                          .element = element, .width = width, .next = c->accesses};
    c->accesses = access;
    return access;
}

enum { POINTER_UNCHECKED, POINTER_INSIDE, POINTER_AT_START };

/* rax holds an address, and unless POINTER_UNCHECKED rsi the pointer it was
 * derived from: both must lie in the record the site last reached, which is
 * still alive. A pointer that always starts its object, as an array does, need
 * only equal that record's base. Leaves the host address in rcx. */
static void cached(Compiler *c, JitAccess *access, int slow, int check_pointer, int write) {
    mov_pointer(c, RDX, access);
    load64(c, R8, R15, EPOCH);
    alu_rm(c, ALU_CMP, 1, R8, RDX, SITE(epoch));
    jcc(c, CC_NE, slow);
    if (check_pointer == POINTER_AT_START) {
        alu_rm(c, ALU_CMP, 1, RSI, RDX, SITE(base));
        jcc(c, CC_NE, slow);
    } else if (check_pointer == POINTER_INSIDE) {
        mov_rr(c, RCX, RSI);
        alu_rm(c, ALU_SUB, 1, RCX, RDX, SITE(base));
        alu_rm(c, ALU_CMP, 1, RCX, RDX, SITE(size));
        jcc(c, CC_A, slow);
    }
    mov_rr(c, RCX, RAX);
    alu_rm(c, ALU_SUB, 1, RCX, RDX, SITE(base));
    alu_rm(c, ALU_CMP, 1, RCX, RDX, SITE(limit));
    jcc(c, CC_A, slow);
    if (!access->width) return;
    if (access->width > 1) {
        emit(c, 0xA8);
        emit(c, (unsigned)(access->width - 1));
        jcc(c, CC_NE, slow);
    }
    if (write) {
        alu_mi(c, ALU_CMP, 1, RDX, SITE(writable), 0);
        jcc(c, CC_E, slow);
    }
    int initialized = new_label(c);
    load64(c, R8, RDX, SITE(initialized));
    test_rr(c, 1, R8, R8);
    jcc(c, CC_E, initialized);
    alu_rr(c, ALU_ADD, 1, R8, RCX);
    switch (access->width) {
        case 1: op_rm(c, 0, 0, 0x80, 7, R8, 0, 0); emit(c, 1); break;
        case 2: op_rm(c, 0x66, 0, 0x81, 7, R8, 0, 0); emit(c, 1); emit(c, 1); break;
        case 4: op_rm(c, 0, 0, 0x81, 7, R8, 0, 0); emit32(c, 0x01010101u); break;
        default:
            mov_imm(c, R9, UINT64_C(0x0101010101010101));
            op_rm(c, 0, 1, 0x39, R9, R8, 0, 0);
            break;
    }
    jcc(c, CC_NE, slow);
    bind(c, initialized);
    alu_rm(c, ALU_ADD, 1, RCX, RDX, SITE(data));
}

static void finish_value(Compiler *c, CtType type) {
    if (is_real(type)) movq_to_xmm(c, 0, RAX);
}

/* rax: address -> rax or xmm0: the value. */
static void access_load(Compiler *c, JitAccess *access) {
    int slow = new_label(c), done = new_label(c), saved;
    cached(c, access, slow, 0, 0);
    load_typed(c, access->element, is_real(access->element) ? 0 : RAX, RCX, 0);
    bind(c, done);
    open_cold(c, slow, &saved);
    mov_rr(c, RDX, RAX);
    mov_pointer(c, RSI, access);
    mov_rr(c, RDI, R15);
    call_helper(c, (Helper)jit_load);
    finish_value(c, access->element);
    close_cold(c, done, saved);
}

/* rax: address, r10 or xmm1: the value, already of the element type. */
static void access_store(Compiler *c, JitAccess *access) {
    int slow = new_label(c), done = new_label(c), saved;
    cached(c, access, slow, 0, 1);
    store_typed(c, access->element, RCX, 0, is_real(access->element) ? 1 : R10);
    bind(c, done);
    open_cold(c, slow, &saved);
    if (is_real(access->element)) movq_from_xmm(c, RCX, 1);
    else mov_rr(c, RCX, R10);
    mov_rr(c, RDX, RAX);
    mov_pointer(c, RSI, access);
    mov_rr(c, RDI, R15);
    call_helper(c, (Helper)jit_store);
    close_cold(c, done, saved);
}

/* rax: pointer, rcx: index -> rsi: pointer, rdi: index, rax: element address. */
static void element_address(Compiler *c, JitAccess *access, size_t size, int slow) {
    mov_rr(c, RSI, RAX);
    mov_rr(c, RDI, RCX);
    if (wide(access->index)) {
        op_rr(c, 0, 1, 0x63, R8, RDI, 0);
        alu_rr(c, ALU_CMP, 1, R8, RDI);
        jcc(c, CC_NE, slow);
    }
    if (size == 1 || size == 2 || size == 4 || size == 8)
        op_sib(c, 1, 0x8D, RAX, RSI, RDI, size == 1 ? 0 : size == 2 ? 1 : size == 4 ? 2 : 3);
    else {
        imul_ri(c, 1, RAX, RDI, (int32_t)size);
        alu_rr(c, ALU_ADD, 1, RAX, RSI);
    }
}

static void element_slow(Compiler *c, JitAccess *access, int slow, int done, Helper helper, CtType result) {
    int saved;
    open_cold(c, slow, &saved);
    mov_rr(c, RDX, RSI);
    mov_rr(c, RCX, RDI);
    mov_pointer(c, RSI, access);
    mov_rr(c, RDI, R15);
    call_helper(c, helper);
    finish_value(c, result);
    close_cold(c, done, saved);
}

/* rax: pointer, rcx: index -> the element's value, or its decayed address for an array. */
static void element_load(Compiler *c, JitAccess *access, size_t size, CtType result, int pointer_check) {
    int slow = new_label(c), done = new_label(c);
    element_address(c, access, size, slow);
    cached(c, access, slow, pointer_check, 0);
    if (access->width) load_typed(c, access->element, is_real(access->element) ? 0 : RAX, RCX, 0);
    bind(c, done);
    element_slow(c, access, slow, done, (Helper)jit_load_element, result);
}

/* rax: pointer, rcx: index -> rax: the element's address, checked as pointer arithmetic. */
static void element_lvalue(Compiler *c, JitAccess *access, size_t size, int pointer_check) {
    int slow = new_label(c), done = new_label(c);
    element_address(c, access, size, slow);
    cached(c, access, slow, pointer_check, 0);
    bind(c, done);
    element_slow(c, access, slow, done, (Helper)jit_element, CT_LONG);
}

/* rax: pointer, rcx: count -> rax: pointer plus or minus count elements. */
static void pointer_offset(Compiler *c, JitAccess *access, int op) {
    size_t size = type_size(type_target(access->pointer));
    int slow = new_label(c), done = new_label(c), saved;
    mov_rr(c, RSI, RAX);
    mov_rr(c, RDI, RCX);
    if (!size) jmp(c, slow);
    else {
        if (wide(access->index)) {
            op_rr(c, 0, 1, 0x63, R8, RDI, 0);
            alu_rr(c, ALU_CMP, 1, R8, RDI);
            jcc(c, CC_NE, slow);
        }
        mov_rr(c, R8, RDI);
        if (size != 1) imul_ri(c, 1, R8, R8, (int32_t)size);
        if (op == '-') group3(c, 3, 1, R8);
        mov_rr(c, RAX, RSI);
        alu_rr(c, ALU_ADD, 1, RAX, R8);
        cached(c, access, slow, POINTER_INSIDE, 0);
    }
    bind(c, done);
    open_cold(c, slow, &saved);
    mov_rr(c, RDX, RSI);
    mov_rr(c, RCX, RDI);
    mov_pointer(c, RSI, access);
    mov_rr(c, RDI, R15);
    mov_imm(c, R8, (uint64_t)op);
    call_helper(c, (Helper)jit_pointer_offset);
    close_cold(c, done, saved);
}

static Local *find_local(Compiler *c, const Node *declaration) {
    for (size_t i = 0; i < c->local_count; ++i)
        if (c->locals[i].declaration == declaration) return &c->locals[i];
    return NULL;
}

static int temp_offset(const Compiler *c, int index) { return -(c->saved_count * 8 + c->local_bytes + 8 * (index + 1)); }

static int temp_push(Compiler *c) {
    int index = c->temp_depth++;
    if (c->temp_depth > c->temp_peak) c->temp_peak = c->temp_depth;
    return temp_offset(c, index);
}

static void temp_pop(Compiler *c) { --c->temp_depth; }

static void spill(Compiler *c, CtType type, int offset) {
    if (is_real(type)) movsd_store(c, RBP, offset, 0);
    else store64(c, RBP, offset, RAX);
}

static void reload(Compiler *c, CtType type, int offset, int gpr, int xmm) {
    if (is_real(type)) movsd_load(c, xmm, RBP, offset);
    else load64(c, gpr, RBP, offset);
}

static void read_local(Compiler *c, Local *local, Node *use, int gpr, int xmm) {
    if (local->flag) {
        op_rm(c, 0, 0, 0x80, 7, RBP, local->flag, 0);
        emit(c, 0);
        jcc(c, CC_E, cold_label(c, use, FAIL_UNINITIALIZED));
    }
    if (is_real(local->type)) {
        if (local->reg >= 0) movapd(c, xmm, local->reg);
        else movsd_load(c, xmm, RBP, local->offset);
    } else if (local->reg >= 0) mov_rr(c, gpr, local->reg);
    else load64(c, gpr, RBP, local->offset);
}

static void write_local(Compiler *c, Local *local, int gpr, int xmm) {
    if (is_real(local->type)) {
        if (local->reg >= 0) movapd(c, local->reg, xmm);
        else movsd_store(c, RBP, local->offset, xmm);
    } else if (local->reg >= 0) mov_rr(c, local->reg, gpr);
    else store64(c, RBP, local->offset, gpr);
    if (local->flag) {
        op_rm(c, 0, 0, 0xC6, 0, RBP, local->flag, 0);
        emit(c, 1);
    }
}

enum { NAME_LOCAL, NAME_GLOBAL, NAME_CONSTANT, NAME_LIBRARY, NAME_FUNCTION };

typedef struct {
    int kind;
    CtType type;
    Local *local;
    Symbol *symbol;
    CtValue constant;
} Name;

static int resolve_name(Compiler *c, Node *node, Name *name) {
    *name = (Name){0};
    if (node->resolution == RESOLVE_LOCAL) {
        Node *declaration = node->cache.use.declaration;
        Local *local = declaration ? find_local(c, declaration) : NULL;
        if (!local) return reject(c, node, "name without a known declaration");
        *name = (Name){.kind = NAME_LOCAL, .type = local->type, .local = local};
        return 1;
    }
    if (node->resolution != RESOLVE_GLOBAL) return reject(c, node, "dynamically resolved name");
    Symbol *symbol = runtime_global(c->interpreter, &node->token);
    if (!symbol) {
        CtType type;
        if ((node->token.length == 5 && !memcmp(node->token.start, "stdin", 5)) ||
            (node->token.length == 6 && (!memcmp(node->token.start, "stdout", 6) || !memcmp(node->token.start, "stderr", 6)))) {
            *name = (Name){.kind = NAME_LIBRARY, .type = type_pointer(CT_VOID)};
            return 1;
        }
        if (builtin_type(node->token, &type)) {
            *name = (Name){.kind = NAME_FUNCTION};
            return 1;
        }
        return reject(c, node, "unknown global");
    }
    if (symbol->function) {
        *name = (Name){.kind = NAME_FUNCTION, .type = symbol->value.type, .symbol = symbol};
        return 1;
    }
    if (!symbol->object || !symbol->object->alive) return reject(c, node, "global without storage");
    if (symbol->enum_constant) {
        *name = (Name){.kind = NAME_CONSTANT, .type = symbol->value.type, .symbol = symbol,
                       .constant = memory_decode(symbol->object->data, symbol->value.type)};
        return supported(symbol->value.type) && type_is_integer(symbol->value.type) ? 1
               : reject(c, node, "unsupported enumeration constant");
    }
    *name = (Name){.kind = NAME_GLOBAL, .type = symbol->value.type, .symbol = symbol};
    if (type_is_array(symbol->value.type)) return 1;
    if (!supported(symbol->value.type)) return reject(c, node, "unsupported global type");
    if (!symbol->object->fully_initialized) return reject(c, node, "global not fully initialized");
    return 1;
}

static CtType type_of(Compiler *c, Node *node);
static CtType object_type(Compiler *c, Node *node);

static const Member *member_of(Compiler *c, Node *node) {
    CtType aggregate = node->through_pointer ? type_of(c, node->left) : object_type(c, node->left);
    if (c->failed) return NULL;
    if (node->through_pointer) {
        if (!type_is_pointer(aggregate)) return reject(c, node, "'->' on a non-pointer"), NULL;
        aggregate = type_target(aggregate);
    }
    if (!type_is_aggregate(aggregate)) return reject(c, node, "member of a non-aggregate"), NULL;
    const Member *member = type_member(aggregate, node->token.start, node->token.length);
    if (!member) return reject(c, node, "unknown member"), NULL;
    return member;
}

static CtType object_type(Compiler *c, Node *node) {
    Name name;
    switch (node->kind) {
        case N_MEMBER: {
            const Member *member = member_of(c, node);
            return member ? member->type : CT_VOID;
        }
        case N_NAME:
            if (!resolve_name(c, node, &name)) return CT_VOID;
            return name.kind == NAME_LOCAL || name.kind == NAME_GLOBAL ? name.type : type_of(c, node);
        case N_INDEX: {
            CtType pointer = type_of(c, node->left);
            if (!type_is_pointer(pointer)) return reject(c, node, "index of a non-pointer"), CT_VOID;
            return type_target(pointer);
        }
        case N_UNARY:
            if (node->token.kind == '*') {
                CtType pointer = type_of(c, node->left);
                if (!type_is_pointer(pointer)) return reject(c, node, "dereference of a non-pointer"), CT_VOID;
                return type_target(pointer);
            }
            return type_of(c, node);
        default: return type_of(c, node);
    }
}

/* Arrays decay; structures and unions are carried by address, as the interpreter carries them. */
static CtType value_type(Compiler *c, Node *node, CtType object) {
    TypeKind kind = type_kind(object);
    if (kind == TY_ARRAY) return type_pointer(type_target(object));
    if (!supported(object) && !type_is_aggregate(object)) return reject(c, node, "unsupported object type"), CT_VOID;
    return object;
}

static int arithmetic(CtType type) { return type_kind(type) <= TY_ULLONG || type == CT_DOUBLE; }

/* A call by the name of a function or a library routine, rather than through a pointer value. */
static int direct_call(Compiler *c, Node *call, Symbol **symbol, Node **function) {
    Node *callee = call->left;
    *symbol = NULL;
    *function = NULL;
    if (!callee || callee->kind != N_NAME || callee->resolution != RESOLVE_GLOBAL) return 0;
    *symbol = runtime_global(c->interpreter, &callee->token);
    if (*symbol && !(*symbol)->function) return 0;
    if (*symbol && (*symbol)->function->right) *function = (*symbol)->function;
    return 1;
}

static Node *pointer_callee(Compiler *c, Node *callee) {
    while (callee->kind == N_UNARY && callee->token.kind == '*') {
        CtType inner = type_of(c, callee->left);
        if (c->failed || !type_is_pointer(inner) || !type_is_function(type_target(inner))) break;
        callee = callee->left;
    }
    return callee;
}

static CtType call_type(Compiler *c, Node *call) {
    Symbol *symbol;
    Node *function;
    if (direct_call(c, call, &symbol, &function)) {
        CtType type;
        if (function) return type_target(function->type);
        if (symbol) return type_target(symbol->function->type);
        if (builtin_type(call->left->token, &type)) return type;
        return reject(c, call, "unknown function"), CT_VOID;
    }
    CtType pointer = type_of(c, pointer_callee(c, call->left));
    if (c->failed) return CT_VOID;
    if (!type_is_pointer(pointer) || !type_is_function(type_target(pointer)))
        return reject(c, call, "call of a non-function"), CT_VOID;
    CtType result = type_target(type_target(pointer));
    return result == CT_VOID || supported(result) ? result : (reject(c, call, "aggregate result"), CT_VOID);
}

static CtType type_of(Compiler *c, Node *node) {
    if (c->failed) return CT_VOID;
    switch (node->kind) {
        case N_VALUE: {
            CtType type = node->token.value.type;
            return supported(type) ? type : (reject(c, node, "unsupported constant"), CT_VOID);
        }
        case N_STRING: return type_pointer(CT_CHAR);
        case N_NAME: {
            Name name;
            if (!resolve_name(c, node, &name)) return CT_VOID;
            if (name.kind == NAME_FUNCTION)
                return name.symbol ? type_pointer(name.type) : (reject(c, node, "library function as a value"), CT_VOID);
            if (name.kind == NAME_CONSTANT || name.kind == NAME_LIBRARY) return name.type;
            return value_type(c, node, name.type);
        }
        case N_CAST:
            if (node->type != CT_VOID && !supported(node->type)) return reject(c, node, "unsupported cast"), CT_VOID;
            return node->type;
        case N_SIZEOF: case N_ALIGNOF: return CT_ULONG;
        case N_COMMA: return type_of(c, node->right);
        case N_CALL: return call_type(c, node);
        case N_INDEX: case N_MEMBER: return value_type(c, node, object_type(c, node));
        case N_UNARY: case N_POSTFIX: {
            int op = node->token.kind;
            if (op == '&') return type_pointer(object_type(c, node->left));
            if (op == '*') return value_type(c, node, object_type(c, node));
            if (op == '!') return CT_INT;
            CtType type = type_of(c, node->left);
            if (op == TK_INCREMENT || op == TK_DECREMENT) return type;
            if (!arithmetic(type)) return reject(c, node, "unary operator on a non-number"), CT_VOID;
            return type_promote(type);
        }
        case N_BINARY: {
            int op = node->token.kind;
            if (is_assignment(op)) return value_type(c, node, object_type(c, node->left));
            CtType left = type_of(c, node->left);
            if (op == TK_SHL || op == TK_SHR) return type_promote(left);
            if (op != '+' && op != '-' && op != '*' && op != '/' && op != '%' && op != '&' && op != '|' && op != '^')
                return CT_INT;
            CtType right = type_of(c, node->right);
            if (type_is_pointer(left) && type_is_pointer(right)) return op == '-' ? CT_LONG : CT_INT;
            if (type_is_pointer(left)) return left;
            if (type_is_pointer(right)) return right;
            if (!arithmetic(left) || !arithmetic(right)) return reject(c, node, "operator on a non-number"), CT_VOID;
            return type_common(left, right);
        }
        case N_CONDITIONAL: {
            CtType a = type_of(c, node->right), b = type_of(c, node->third);
            if (type_is_aggregate(a) || type_is_aggregate(b))
                return a == b ? a : (reject(c, node, "mismatched aggregates"), CT_VOID);
            CtType type = type_is_pointer(a) || a == CT_VOID ? a : type_is_pointer(b) || b == CT_VOID ? b : type_common(a, b);
            if (type == CT_FLOAT) return reject(c, node, "float"), CT_VOID;
            return type;
        }
        default: return reject(c, node, "unsupported expression"), CT_VOID;
    }
}

static CtType gen(Compiler *c, Node *node);
static void branch(Compiler *c, Node *node, int when, int label);

/* A constant or a private local: one move puts it in any register, without a helper call. */
static int simple(Compiler *c, Node *node) {
    if (node->kind == N_VALUE) return supported(node->token.value.type);
    if (node->kind != N_NAME || node->resolution != RESOLVE_LOCAL || !node->cache.use.declaration) return 0;
    Local *local = find_local(c, node->cache.use.declaration);
    return local && local->storage == LOCAL_SLOT;
}

/* A private integer or pointer local held in a register with nothing to check before reading it. */
static Local *register_local(Compiler *c, Node *node) {
    if (node->kind != N_NAME || node->resolution != RESOLVE_LOCAL || !node->cache.use.declaration) return NULL;
    Local *local = find_local(c, node->cache.use.declaration);
    return local && local->storage == LOCAL_SLOT && local->reg >= 0 && !local->flag && !is_real(local->type) ? local : NULL;
}

static CtType gen_simple(Compiler *c, Node *node, int gpr, int xmm) {
    if (node->kind == N_VALUE) {
        CtValue value = node->token.value;
        if (is_real(value.type)) load_real_constant(c, xmm, value.as.real);
        else mov_imm(c, gpr, value.as.unsigned_integer);
        return value.type;
    }
    Local *local = find_local(c, node->cache.use.declaration);
    read_local(c, local, node, gpr, xmm);
    return local->type;
}

static void check_finite(Compiler *c, Node *node) {
    movq_from_xmm(c, R11, 0);
    alu_rr(c, ALU_ADD, 1, R11, R11);
    alu_rm(c, ALU_CMP, 1, R11, RIP, constant(c, UINT64_C(0xFFE0000000000000)));
    jcc(c, CC_AE, cold_label(c, node, FAIL_NOT_FINITE));
}

static int comparison(int op) {
    return op == '<' || op == '>' || op == TK_LE || op == TK_GE || op == TK_EQ || op == TK_NE;
}

static int condition_code(int op, int is_signed, int real) {
    switch (op) {
        case '<': return real || !is_signed ? CC_B : CC_L;
        case '>': return real || !is_signed ? CC_A : CC_G;
        case TK_LE: return real || !is_signed ? CC_BE : CC_LE;
        case TK_GE: return real || !is_signed ? CC_AE : CC_GE;
        case TK_EQ: return CC_E;
        default: return CC_NE;
    }
}

/* Left in rax or xmm0 and right in rcx or xmm1, both of the type the operation uses. */
static void apply(Compiler *c, int op, Node *node, CtType type, int when, int label) {
    int right = c->right_operand ? c->right_operand : RCX;
    c->right_operand = 0;
    if (comparison(op)) {
        int real = is_real(type);
        if (real) ucomisd(c, 0, 1);
        else alu_rr(c, ALU_CMP, 1, RAX, right);
        int cc = condition_code(op, !real && type_kind(type) != TY_POINTER && type_is_signed(type), real);
        if (label >= 0) jcc(c, when ? cc : cc ^ 1, label);
        else setcc(c, cc, RAX);
        return;
    }
    if (is_real(type)) {
        switch (op) {
            case '+': sse_rr(c, 0xF2, 0x58, 0, 1); break;
            case '-': sse_rr(c, 0xF2, 0x5C, 0, 1); break;
            case '*': sse_rr(c, 0xF2, 0x59, 0, 1); break;
            case '/':
                sse_rr(c, 0x66, 0x57, 15, 15);
                ucomisd(c, 1, 15);
                jcc(c, CC_E, cold_label(c, node, FAIL_DIVISION));
                sse_rr(c, 0xF2, 0x5E, 0, 1);
                break;
            default: (void)reject(c, node, "operator requires integer operands"); return;
        }
        check_finite(c, node);
        return;
    }
    int is_signed = type_is_signed(type), w = wide(type);
    switch (op) {
        case '+': case '-': case '*':
            if (op == '*') imul_rr(c, w, RAX, right);
            else alu_rr(c, op == '+' ? ALU_ADD : ALU_SUB, w, RAX, right);
            if (is_signed) jcc(c, CC_O, cold_label(c, node, FAIL_OVERFLOW));
            if (is_signed && !w) canonical(c, type, RAX);
            return;
        case '&': alu_rr(c, ALU_AND, 1, RAX, right); return;
        case '|': alu_rr(c, ALU_OR, 1, RAX, right); return;
        case '^': alu_rr(c, ALU_XOR, 1, RAX, right); return;
        case '/': case '%': {
            mov_rr(c, RCX, right);
            test_rr(c, w, RCX, RCX);
            jcc(c, CC_E, cold_label(c, node, FAIL_DIVISION));
            if (is_signed) {
                int fine = new_label(c);
                alu_ri(c, ALU_CMP, w, RCX, -1);
                jcc(c, CC_NE, fine);
                if (w) {
                    mov_imm(c, RDX, UINT64_C(1) << 63);
                    alu_rr(c, ALU_CMP, 1, RAX, RDX);
                } else alu_ri(c, ALU_CMP, 0, RAX, INT32_MIN);
                jcc(c, CC_E, cold_label(c, node, FAIL_OVERFLOW));
                bind(c, fine);
                if (w) emit(c, 0x48);
                emit(c, 0x99);
                group3(c, 7, w, RCX);
            } else {
                alu_rr(c, ALU_XOR, 0, RDX, RDX);
                group3(c, 6, w, RCX);
            }
            if (op == '%') mov_rr(c, RAX, RDX);
            if (!w) canonical(c, type, RAX);
            return;
        }
        default: (void)reject(c, node, "unsupported operator"); return;
    }
}

static void shift(Compiler *c, int op, Node *node, CtType type) {
    int width = (int)type_size(type) * 8, w = wide(type);
    alu_ri(c, ALU_CMP, 1, RCX, width);
    jcc(c, CC_AE, cold_label(c, node, FAIL_SHIFT));
    if (op == TK_SHR) {
        shift_cl(c, type_is_signed(type) ? 7 : 5, 1, RAX);
        return;
    }
    if (!type_is_signed(type)) {
        shift_cl(c, 4, w, RAX);
        return;
    }
    test_rr(c, 1, RAX, RAX);
    jcc(c, CC_S, cold_label(c, node, FAIL_NEGATIVE_SHIFT));
    if (w) {
        mov_imm(c, RDX, INT64_MAX);
        shift_cl(c, 5, 1, RDX);
        alu_rr(c, ALU_CMP, 1, RAX, RDX);
        jcc(c, CC_G, cold_label(c, node, FAIL_OVERFLOW));
        shift_cl(c, 4, 1, RAX);
    } else {
        shift_cl(c, 4, 1, RAX);
        mov_imm(c, RDX, INT32_MAX);
        alu_rr(c, ALU_CMP, 1, RAX, RDX);
        jcc(c, CC_G, cold_label(c, node, FAIL_OVERFLOW));
    }
}

/* The constant's value converted to type, when an instruction can carry it as a 32-bit immediate. */
static int immediate_of(Node *node, CtType type, int64_t *value) {
    if (node->kind != N_VALUE || !type_is_integer(node->token.value.type) || !type_is_integer(type)) return 0;
    uint64_t bits = node->token.value.as.unsigned_integer;
    if (type_size(type) < 8) {
        bits &= type_mask(type);
        if (type_is_signed(type) && bits > (uint64_t)type_maximum(type)) bits -= type_mask(type) + 1;
    }
    *value = (int64_t)bits;
    return fits32(*value);
}

static int power_of_two(int64_t value) { return value > 0 && !(value & (value - 1)); }

static int log2_of(int64_t value) {
    int k = 0;
    while ((INT64_C(1) << k) < value) ++k;
    return k;
}

static void shift_immediate(Compiler *c, int extension, int reg, int count) {
    op_rr(c, 0, 1, 0xC1, extension, reg, 0);
    emit(c, (unsigned)count);
}

/* Left in rax, of the type; returns 0 when the operation needs its operand in a register. */
static int apply_immediate(Compiler *c, int op, Node *node, CtType type, int64_t value, int when, int label) {
    int is_signed = type_is_signed(type), w = wide(type);
    int32_t immediate = (int32_t)value;
    if (comparison(op)) {
        alu_ri(c, ALU_CMP, 1, RAX, immediate);
        int cc = condition_code(op, is_signed, 0);
        if (label >= 0) jcc(c, when ? cc : cc ^ 1, label);
        else setcc(c, cc, RAX);
        return 1;
    }
    switch (op) {
        case '+': case '-': case '*':
            if (op == '*') imul_ri(c, w, RAX, RAX, immediate);
            else alu_ri(c, op == '+' ? ALU_ADD : ALU_SUB, w, RAX, immediate);
            if (is_signed) jcc(c, CC_O, cold_label(c, node, FAIL_OVERFLOW));
            if (is_signed && !w) canonical(c, type, RAX);
            return 1;
        case '&': alu_ri(c, ALU_AND, 1, RAX, immediate); return 1;
        case '|': alu_ri(c, ALU_OR, 1, RAX, immediate); return 1;
        case '^': alu_ri(c, ALU_XOR, 1, RAX, immediate); return 1;
        case '/': case '%': {
            if (!power_of_two(value)) return 0;
            int k = log2_of(value);
            if (!is_signed) {
                if (op == '/') { if (k) shift_immediate(c, 5, RAX, k); }
                else alu_ri(c, ALU_AND, 1, RAX, (int32_t)(value - 1));
                return 1;
            }
            if (!k) {
                if (op == '%') mov_imm(c, RAX, 0);
                return 1;
            }
            mov_rr(c, RDX, RAX);
            shift_immediate(c, 7, RDX, 63);
            shift_immediate(c, 5, RDX, 64 - k);
            if (op == '/') {
                alu_rr(c, ALU_ADD, 1, RAX, RDX);
                shift_immediate(c, 7, RAX, k);
            } else {
                lea(c, RCX, RAX, 0);
                alu_rr(c, ALU_ADD, 1, RCX, RDX);
                alu_ri(c, ALU_AND, 1, RCX, (int32_t)-value);
                alu_rr(c, ALU_SUB, 1, RAX, RCX);
            }
            return 1;
        }
        default: return 0;
    }
}

/* The left operand is in rax or xmm0, or with left >= 0 in that temporary. */
static CtType operate(Compiler *c, int op, Node *node, CtType left_type, int left, Node *right, int when, int label) {
    CtType right_type = type_of(c, right);
    if (c->failed) return CT_VOID;
    int pointers = type_is_pointer(left_type) || type_is_pointer(right_type);
    CtType left_target = left_type, right_target = right_type, result = CT_INT;
    if (op == TK_SHL || op == TK_SHR) {
        if (!type_is_integer(left_type) || !type_is_integer(right_type)) return reject(c, node, "shift of a non-integer"), CT_VOID;
        left_target = result = type_promote(left_type);
        right_target = type_promote(right_type);
    } else if (!pointers) {
        if (!arithmetic(left_type) || !arithmetic(right_type)) return reject(c, node, "operator on a non-number"), CT_VOID;
        left_target = right_target = type_common(left_type, right_type);
        result = comparison(op) ? CT_INT : left_target;
    }
    int64_t immediate;
    if (!pointers && op != TK_SHL && op != TK_SHR && !is_real(left_target) && immediate_of(right, right_target, &immediate)) {
        if (left >= 0) reload(c, left_type, left, RAX, 0);
        convert(c, left_type, left_target, 0, node, RAX, 0);
        if (c->failed) return CT_VOID;
        if (apply_immediate(c, op, node, left_target, immediate, when, label)) return result;
        mov_imm(c, RCX, (uint64_t)immediate);
        apply(c, op, node, left_target, when, label);
        return result;
    }
    Local *in_register = register_local(c, right);
    if (in_register && !pointers && op != TK_SHL && op != TK_SHR && type_is_integer(right_target) &&
        integer_fits(in_register->type, right_target)) {
        if (left >= 0) reload(c, left_type, left, RAX, 0);
        convert(c, left_type, left_target, 0, node, RAX, 0);
        if (c->failed) return CT_VOID;
        c->right_operand = in_register->reg;
        apply(c, op, node, left_target, when, label);
        return result;
    }
    if (simple(c, right)) {
        CtType loaded = gen_simple(c, right, RCX, 1);
        convert(c, loaded, right_target, 0, right, RCX, 1);
        if (left >= 0) reload(c, left_type, left, RAX, 0);
    } else {
        if (left < 0) {
            left = temp_push(c);
            spill(c, left_type, left);
        } else temp_push(c);
        CtType generated = gen(c, right);
        if (generated != right_type) return reject(c, right, "inconsistent type"), CT_VOID;
        convert(c, right_type, right_target, 0, right, RAX, 0);
        if (is_real(right_target)) movapd(c, 1, 0);
        else mov_rr(c, RCX, RAX);
        reload(c, left_type, left, RAX, 0);
        temp_pop(c);
    }
    convert(c, left_type, left_target, 0, node, RAX, 0);
    if (c->failed) return CT_VOID;
    if (op == TK_SHL || op == TK_SHR) {
        shift(c, op, node, result);
        return result;
    }
    if (!pointers) {
        apply(c, op, node, left_target, when, label);
        return result;
    }
    if (type_is_pointer(left_type) && type_is_pointer(right_type) && (op == TK_EQ || op == TK_NE)) {
        apply(c, op, node, left_type, when, label);
        return CT_INT;
    }
    int null_right = right->kind == N_VALUE && type_is_integer(right_type) && !right->token.value.as.integer;
    if ((op == TK_EQ || op == TK_NE) && null_right) {
        apply(c, op, node, left_type, when, label);
        return CT_INT;
    }
    if ((op == '+' || op == '-') && type_is_pointer(left_type) && type_is_integer(right_type)) {
        JitAccess *access = new_access(c, node, left_type, right_type, type_target(left_type), 0);
        if (access) pointer_offset(c, access, op);
        return left_type;
    }
    if (op == '+' && type_is_integer(left_type) && type_is_pointer(right_type)) {
        mov_rr(c, R11, RAX);
        mov_rr(c, RAX, RCX);
        mov_rr(c, RCX, R11);
        JitAccess *access = new_access(c, node, right_type, left_type, type_target(right_type), 0);
        if (access) pointer_offset(c, access, '+');
        return right_type;
    }
    CtType produced = op == '-' && type_is_pointer(left_type) && type_is_pointer(right_type) ? CT_LONG
                    : comparison(op) ? CT_INT : type_is_pointer(left_type) ? left_type : right_type;
    JitAccess *access = new_access(c, node, left_type, right_type, produced, 0);
    if (!access) return CT_VOID;
    mov_rr(c, RDX, RAX);
    mov_pointer(c, RSI, access);
    mov_rr(c, RDI, R15);
    mov_imm(c, R8, (uint64_t)op);
    call_helper(c, (Helper)jit_pointer_binary);
    if (label >= 0) {
        test_rr(c, 1, RAX, RAX);
        jcc(c, when ? CC_NE : CC_E, label);
    }
    return produced;
}

enum { PLACE_LOCAL, PLACE_HOST, PLACE_MEMORY };

typedef struct {
    int kind;
    Local *local;
    void *host;
    CtType type;
    int address, checked; /* temporaries: the object's address, and its host address once checked, or zero */
    JitAccess *access;
    int readonly;
} Lplace;

/* Whether evaluating it can call anything, which alone could end an object's lifetime. */
static int calls_out(const Node *node) {
    for (; node; node = node->next) {
        if (node->kind == N_CALL) return 1;
        if (calls_out(node->left) || calls_out(node->right) || calls_out(node->third) || calls_out(node->fourth)) return 1;
    }
    return 0;
}

static int static_address(Compiler *c, Local *local) {
    mov_pointer(c, RAX, &local->declaration->address);
    load64(c, RAX, RAX, 0);
    return 1;
}

static int base_check(Compiler *c, Node *pointer);

/* The address of a local, global or element object, in rax. */
static int object_address(Compiler *c, Node *node, CtType *object) {
    Name name;
    if (node->kind == N_NAME) {
        if (!resolve_name(c, node, &name)) return 0;
        *object = name.type;
        if (name.kind == NAME_GLOBAL) {
            mov_imm(c, RAX, name.symbol->address);
            return 1;
        }
        if (name.kind != NAME_LOCAL || name.local->storage == LOCAL_SLOT) return reject(c, node, "address of a private local");
        if (name.local->storage == LOCAL_STATIC) return static_address(c, name.local);
        load64(c, RAX, RBP, name.local->offset);
        return 1;
    }
    if (node->kind == N_UNARY && node->token.kind == '*') {
        CtType pointer = gen(c, node->left);
        if (!type_is_pointer(pointer)) return reject(c, node, "dereference of a non-pointer");
        *object = type_target(pointer);
        return 1;
    }
    if (node->kind == N_MEMBER) {
        const Member *member = member_of(c, node);
        if (!member) return 0;
        CtType base;
        if (node->through_pointer) {
            if (!type_is_pointer(gen(c, node->left))) return reject(c, node, "'->' on a non-pointer");
        } else if (!object_address(c, node->left, &base)) return 0;
        if (member->offset) alu_ri(c, ALU_ADD, 1, RAX, (int32_t)member->offset);
        *object = member->type;
        return 1;
    }
    if (node->kind == N_INDEX) {
        CtType pointer = type_of(c, node->left), index = type_of(c, node->right);
        if (c->failed || !type_is_pointer(pointer) || !type_is_integer(index)) return reject(c, node, "unsupported index");
        CtType element = type_target(pointer);
        size_t size = type_size(element);
        if (!size || type_is_function(element)) return reject(c, node, "index of an incomplete element");
        if (simple(c, node->right)) {
            if (gen(c, node->left) != pointer) return reject(c, node, "inconsistent type");
            gen_simple(c, node->right, RCX, 1);
        } else {
            if (gen(c, node->left) != pointer) return reject(c, node, "inconsistent type");
            int saved = temp_push(c);
            spill(c, pointer, saved);
            if (gen(c, node->right) != index) return reject(c, node, "inconsistent type");
            mov_rr(c, RCX, RAX);
            reload(c, pointer, saved, RAX, 0);
            temp_pop(c);
        }
        size_t width = supported(element) ? size : 0;
        JitAccess *access = new_access(c, node, pointer, index, element, width);
        if (!access) return 0;
        element_lvalue(c, access, size, base_check(c, node->left));
        *object = element;
        return 1;
    }
    return reject(c, node, "not an object");
}

static View *find_view(Compiler *c, Node *pointer);
static void view_pointer(Compiler *c, View *view);
static void view_element(Compiler *c, View *view, Node *index_node, CtType index, int store, int miss);
static void recover_index(Compiler *c, Node *index_node);
static int gen_index_into_rcx(Compiler *c, Node *index_node, CtType index);

static int lvalue(Compiler *c, Node *node, Node *operation, Lplace *place, int stable) {
    *place = (Lplace){0};
    Name name;
    if (node->kind == N_NAME) {
        if (!resolve_name(c, node, &name)) return 0;
        if (name.kind == NAME_LOCAL && name.local->storage == LOCAL_SLOT) {
            *place = (Lplace){.kind = PLACE_LOCAL, .local = name.local, .type = name.type, .readonly = name.local->readonly};
            return 1;
        }
        if (name.kind == NAME_GLOBAL && supported(name.type)) {
            *place = (Lplace){.kind = PLACE_HOST, .host = name.symbol->object->data, .type = name.type,
                              .readonly = name.symbol->is_const || name.symbol->object->readonly};
            return 1;
        }
    }
    View *view = stable && node->kind == N_INDEX ? find_view(c, node->left) : NULL;
    if (view) {
        CtType pointer = type_of(c, node->left), index = type_of(c, node->right);
        if (c->failed || !type_is_integer(index)) return reject(c, node, "unsupported index");
        if (!gen_index_into_rcx(c, node->right, index)) return 0;
        int miss = new_label(c), done = new_label(c), saved;
        *place = (Lplace){.kind = PLACE_MEMORY, .type = view->element};
        place->address = temp_push(c);
        place->checked = temp_push(c);
        view_element(c, view, node->right, index, 1, miss);
        store64(c, RBP, place->checked, RCX);
        bind(c, done);
        open_cold(c, miss, &saved);
        view_pointer(c, view);
        recover_index(c, node->right);
        JitAccess *element = new_access(c, node, pointer, index, view->element, view->width);
        if (element) element_lvalue(c, element, view->width, base_check(c, node->left));
        store64(c, RBP, place->address, RAX);
        alu_mi(c, ALU_AND, 1, RBP, place->checked, 0);
        close_cold(c, done, saved);
        place->access = new_access(c, operation, type_pointer(view->element), CT_INT, view->element, view->width);
        return place->access != NULL;
    }
    CtType object;
    if (!object_address(c, node, &object)) return 0;
    if (!supported(object)) return reject(c, node, "assignment to an unsupported object");
    place->kind = PLACE_MEMORY;
    place->type = object;
    place->address = temp_push(c);
    store64(c, RBP, place->address, RAX);
    place->access = new_access(c, operation, type_pointer(object), CT_INT, object, type_size(object));
    if (!place->access) return 0;
    if (stable) {
        int miss = new_label(c), done = new_label(c), saved;
        place->checked = temp_push(c);
        cached(c, place->access, miss, POINTER_UNCHECKED, 1);
        store64(c, RBP, place->checked, RCX);
        bind(c, done);
        open_cold(c, miss, &saved);
        alu_mi(c, ALU_AND, 1, RBP, place->checked, 0);
        close_cold(c, done, saved);
    }
    return 1;
}

static void release_place(Compiler *c, Lplace *place) {
    if (place->checked) temp_pop(c);
    if (place->kind == PLACE_MEMORY) temp_pop(c);
}

static void load_place(Compiler *c, Lplace *place, Node *use) {
    switch (place->kind) {
        case PLACE_LOCAL: read_local(c, place->local, use, RAX, 0); break;
        case PLACE_HOST:
            mov_pointer(c, RCX, place->host);
            load_typed(c, place->type, is_real(place->type) ? 0 : RAX, RCX, 0);
            break;
        default: {
            int generic = -1, done = new_label(c), saved = c->section;
            if (place->checked) {
                generic = new_label(c);
                load64(c, RCX, RBP, place->checked);
                test_rr(c, 1, RCX, RCX);
                jcc(c, CC_E, generic);
                load_typed(c, place->type, is_real(place->type) ? 0 : RAX, RCX, 0);
                bind(c, done);
                open_cold(c, generic, &saved);
            }
            load64(c, RAX, RBP, place->address);
            access_load(c, place->access);
            if (place->checked) close_cold(c, done, saved);
            else bind(c, done);
            break;
        }
    }
}

/* The value, already converted, is in rax or xmm0, and stays there. */
static void store_place(Compiler *c, Lplace *place, Node *node) {
    if (place->readonly) {
        jmp(c, cold_label(c, node, FAIL_CONST));
        return;
    }
    switch (place->kind) {
        case PLACE_LOCAL: write_local(c, place->local, RAX, 0); break;
        case PLACE_HOST:
            mov_pointer(c, RCX, place->host);
            store_typed(c, place->type, RCX, 0, is_real(place->type) ? 0 : RAX);
            break;
        default: {
            int generic = -1, done = new_label(c), saved = c->section;
            if (place->checked) {
                generic = new_label(c);
                load64(c, RCX, RBP, place->checked);
                test_rr(c, 1, RCX, RCX);
                jcc(c, CC_E, generic);
                store_typed(c, place->type, RCX, 0, is_real(place->type) ? 0 : RAX);
                bind(c, done);
                open_cold(c, generic, &saved);
            }
            int value = temp_push(c);
            spill(c, place->type, value);
            if (is_real(place->type)) movapd(c, 1, 0);
            else mov_rr(c, R10, RAX);
            load64(c, RAX, RBP, place->address);
            access_store(c, place->access);
            reload(c, place->type, value, RAX, 0);
            temp_pop(c);
            if (place->checked) close_cold(c, done, saved);
            else bind(c, done);
            break;
        }
    }
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

static int constant_object(Compiler *c, Node *node) {
    Name name;
    if (node->kind != N_NAME || !resolve_name(c, node, &name)) return 0;
    if (name.kind == NAME_LOCAL) return name.local->readonly;
    return name.kind == NAME_GLOBAL && (name.symbol->is_const || name.symbol->object->readonly);
}

static void copy_aggregate(Compiler *c, Node *node, CtType type, int destination) {
    mov_rr(c, RCX, RAX);
    load64(c, RDX, RBP, destination);
    mov_pointer(c, RSI, node);
    mov_rr(c, RDI, R15);
    mov_imm(c, R8, (uint64_t)type);
    int source = temp_push(c);
    store64(c, RBP, source, RAX);
    call_helper(c, (Helper)jit_copy);
    load64(c, RAX, RBP, source);
    temp_pop(c);
}

static CtType assign_aggregate(Compiler *c, Node *node, CtType type) {
    if (node->token.kind != '=') return reject(c, node, "compound assignment to an aggregate"), CT_VOID;
    if (constant_object(c, node->left)) {
        jmp(c, cold_label(c, node, FAIL_CONST));
        return type;
    }
    CtType object;
    if (!object_address(c, node->left, &object)) return CT_VOID;
    int destination = temp_push(c);
    store64(c, RBP, destination, RAX);
    if (gen(c, node->right) != type) return reject(c, node, "aggregate assignment of another type"), CT_VOID;
    copy_aggregate(c, node, type, destination);
    temp_pop(c);
    return type;
}

/* gen() and convert(), with a constant converted while compiling. */
static void gen_as(Compiler *c, Node *node, CtType target, Node *at) {
    CtValue value = node->token.value;
    if (node->kind == N_VALUE && type_is_integer(value.type) && type_is_integer(target) && type_kind(target) != TY_BOOL) {
        int64_t bits;
        if (immediate_of(node, target, &bits) || type_size(target) == 8) {
            mov_imm(c, RAX, type_size(target) == 8 ? value.as.unsigned_integer : (uint64_t)bits);
            return;
        }
    }
    if (node->kind == N_VALUE && type_is_integer(value.type) && target == CT_DOUBLE) {
        load_real_constant(c, 0, type_is_signed(value.type) ? (double)value.as.integer : (double)value.as.unsigned_integer);
        return;
    }
    CtType type = gen(c, node);
    convert(c, type, target, 0, at, RAX, 0);
}

/* element = constant or local: the fast path keeps the element's host address in rcx throughout. */
static CtType assign_view(Compiler *c, Node *node, View *view) {
    Node *target = node->left;
    CtType pointer = type_of(c, target->left), index = type_of(c, target->right), element = view->element;
    if (c->failed) return CT_VOID;
    if (!type_is_integer(index)) return reject(c, node, "unsupported index"), CT_VOID;
    if (!gen_index_into_rcx(c, target->right, index)) return CT_VOID;
    int miss = new_label(c), done = new_label(c), saved;
    view_element(c, view, target->right, index, 1, miss);
    gen_as(c, node->right, element, node);
    store_typed(c, element, RCX, 0, is_real(element) ? 0 : RAX);
    bind(c, done);
    open_cold(c, miss, &saved);
    view_pointer(c, view);
    recover_index(c, target->right);
    JitAccess *at = new_access(c, target, pointer, index, element, view->width);
    JitAccess *store = new_access(c, node, type_pointer(element), CT_INT, element, view->width);
    if (!at || !store) return CT_VOID;
    element_lvalue(c, at, view->width, base_check(c, target->left));
    int address = temp_push(c), value = temp_push(c);
    store64(c, RBP, address, RAX);
    gen_as(c, node->right, element, node);
    spill(c, element, value);
    if (is_real(element)) movapd(c, 1, 0);
    else mov_rr(c, R10, RAX);
    load64(c, RAX, RBP, address);
    access_store(c, store);
    reload(c, element, value, RAX, 0);
    temp_pop(c);
    temp_pop(c);
    close_cold(c, done, saved);
    return element;
}

static CtType assign(Compiler *c, Node *node) {
    int op = node->token.kind;
    CtType target = object_type(c, node->left);
    if (c->failed) return CT_VOID;
    if (type_is_aggregate(target)) return assign_aggregate(c, node, target);
    if (op == '=' && node->left->kind == N_INDEX && simple(c, node->right)) {
        View *view = find_view(c, node->left->left);
        if (view) return assign_view(c, node, view);
    }
    Lplace place;
    if (!lvalue(c, node->left, node, &place, !calls_out(node->right))) return CT_VOID;
    if (op == '=') gen_as(c, node->right, place.type, node);
    else {
        load_place(c, &place, node);
        int left = -1;
        if (!simple(c, node->right)) {
            left = temp_push(c);
            spill(c, place.type, left);
        }
        CtType type = operate(c, base_operator(op), node, place.type, left, node->right, 0, -1);
        if (left >= 0) temp_pop(c);
        convert(c, type, place.type, 0, node, RAX, 0);
    }
    store_place(c, &place, node);
    release_place(c, &place);
    return place.type;
}

static CtType step(Compiler *c, Node *node) {
    int op = node->token.kind == TK_INCREMENT ? '+' : '-';
    int keep_old = node->kind == N_POSTFIX && !c->discarding;
    c->discarding = 0;
    Lplace place;
    if (!lvalue(c, node->left, node, &place, 1)) return CT_VOID;
    CtType type = place.type;
    load_place(c, &place, node);
    int old = temp_push(c);
    if (keep_old) spill(c, type, old);
    if (type_is_pointer(type)) {
        mov_imm(c, RCX, 1);
        JitAccess *access = new_access(c, node, type, CT_INT, type_target(type), 0);
        if (access) pointer_offset(c, access, op);
    } else if (is_real(type)) {
        load_real_constant(c, 1, 1.0);
        apply(c, op, node, CT_DOUBLE, 0, -1);
    } else {
        CtType common = type_common(type, CT_INT);
        convert(c, type, common, 0, node, RAX, 0);
        (void)apply_immediate(c, op, node, common, 1, 0, -1);
        convert(c, common, type, 0, node, RAX, 0);
    }
    store_place(c, &place, node);
    if (keep_old) reload(c, type, old, RAX, 0);
    temp_pop(c);
    release_place(c, &place);
    return type;
}

static void assume_library(Compiler *c, Node *name) {
    if (c->assumed_count == c->assumed_capacity) {
        size_t capacity = c->assumed_capacity ? 2 * c->assumed_capacity : 8;
        Node **grown = realloc(c->assumed, capacity * sizeof *grown);
        if (!grown) { (void)reject(c, name, "out of memory"); return; }
        c->assumed = grown;
        c->assumed_capacity = capacity;
    }
    c->assumed[c->assumed_count++] = name;
}

static JitCode *code_for(CtInterpreter *interpreter, Node *function);

static void outgoing_values(Compiler *c, size_t count, const int *temps, const CtType *types) {
    if ((int)(16 * count) > c->outgoing_peak) c->outgoing_peak = (int)(16 * count);
    for (size_t i = 0; i < count; ++i) {
        op_rm(c, 0, 0, 0xC7, 0, RSP, (int32_t)(16 * i), 0);
        emit32(c, (uint32_t)types[i]);
        load64(c, RAX, RBP, temps[i]);
        store64(c, RSP, (int32_t)(16 * i + 8), RAX);
    }
}

/* Library routines common in hot loops, done inline. Anything unusual takes
 * the library call itself, so that its diagnostics stay exact. */
enum { INTRINSIC_NONE, INTRINSIC_CTYPE, INTRINSIC_ABS, INTRINSIC_LABS, INTRINSIC_SQRT, INTRINSIC_FABS };

static const char *const ctype_names[] = {"isdigit", "isalpha", "isalnum", "isspace", "isupper", "islower", "ispunct",
                                          "isprint", "isgraph", "iscntrl", "isxdigit", "isblank", "toupper", "tolower"};
#define CTYPE_COUNT (sizeof ctype_names / sizeof ctype_names[0])

static int ctype_result(size_t which, int value) {
    switch (which) {
        case 0: return isdigit(value);
        case 1: return isalpha(value);
        case 2: return isalnum(value);
        case 3: return isspace(value);
        case 4: return isupper(value);
        case 5: return islower(value);
        case 6: return ispunct(value);
        case 7: return isprint(value);
        case 8: return isgraph(value);
        case 9: return iscntrl(value);
        case 10: return isxdigit(value);
        case 11: return value == ' ' || value == '\t';
        case 12: return toupper(value);
        default: return tolower(value);
    }
}

/* Indexed by the argument plus one, so EOF comes first. */
static const int32_t *ctype_table(size_t which) {
    static int32_t tables[CTYPE_COUNT][UCHAR_MAX + 2];
    static int ready;
    if (!ready) {
        for (size_t k = 0; k < CTYPE_COUNT; ++k)
            for (int value = EOF; value <= UCHAR_MAX; ++value) tables[k][value + 1] = ctype_result(k, value);
        ready = 1;
    }
    return tables[which];
}

static int intrinsic_kind(Node *node, CtType argument, CtType result, size_t *which) {
    const Token *name = &node->left->token;
    for (size_t k = 0; k < CTYPE_COUNT; ++k)
        if (name->length == strlen(ctype_names[k]) && !memcmp(name->start, ctype_names[k], name->length)) {
            *which = k;
            return type_is_integer(argument) && result == CT_INT ? INTRINSIC_CTYPE : INTRINSIC_NONE;
        }
    int fits_int = type_is_integer(argument) && (type_size(argument) < 4 || argument == CT_INT);
    int fits_long = type_is_integer(argument) && (type_is_signed(argument) || type_size(argument) < 8);
    int number = type_is_integer(argument) || argument == CT_DOUBLE;
    if (name->length == 3 && !memcmp(name->start, "abs", 3)) return fits_int && result == CT_INT ? INTRINSIC_ABS : INTRINSIC_NONE;
    if (name->length == 4 && !memcmp(name->start, "labs", 4)) return fits_long && result == CT_LONG ? INTRINSIC_LABS : INTRINSIC_NONE;
    if (name->length == 4 && !memcmp(name->start, "sqrt", 4)) return number && result == CT_DOUBLE ? INTRINSIC_SQRT : INTRINSIC_NONE;
    if (name->length == 4 && !memcmp(name->start, "fabs", 4)) return number && result == CT_DOUBLE ? INTRINSIC_FABS : INTRINSIC_NONE;
    return INTRINSIC_NONE;
}

static void finite_or(Compiler *c, int label) {
    movq_from_xmm(c, R11, 0);
    alu_rr(c, ALU_ADD, 1, R11, R11);
    alu_rm(c, ALU_CMP, 1, R11, RIP, constant(c, UINT64_C(0xFFE0000000000000)));
    jcc(c, CC_AE, label);
}

/* The argument waits in its temporary; the result is left in rax or xmm0. */
static void intrinsic(Compiler *c, int kind, size_t which, CtType argument, int temp, int fallback) {
    load64(c, RAX, RBP, temp);
    switch (kind) {
        case INTRINSIC_CTYPE:
            lea(c, RCX, RAX, 1);
            alu_ri(c, ALU_CMP, 1, RCX, UCHAR_MAX + 1);
            jcc(c, CC_A, fallback);
            mov_pointer(c, R8, ctype_table(which));
            op_sib(c, 1, 0x63, RAX, R8, RCX, 2);
            return;
        case INTRINSIC_ABS: case INTRINSIC_LABS:
            mov_rr(c, RCX, RAX);
            shift_immediate(c, 7, RCX, 63);
            alu_rr(c, ALU_XOR, 1, RAX, RCX);
            alu_rr(c, ALU_SUB, 1, RAX, RCX);
            if (kind == INTRINSIC_ABS) {
                alu_ri(c, ALU_CMP, 1, RAX, INT32_MAX);
                jcc(c, CC_A, fallback);
            } else {
                test_rr(c, 1, RAX, RAX);
                jcc(c, CC_S, fallback);
            }
            return;
        default:
            if (is_real(argument)) movq_to_xmm(c, 0, RAX);
            else integer_to_real(c, argument, RAX, 0);
            if (kind == INTRINSIC_SQRT) {
                sse_rr(c, 0x66, 0x57, 15, 15);
                ucomisd(c, 0, 15);
                jcc(c, CC_B, fallback);
                sse_rr(c, 0xF2, 0x51, 0, 0);
            } else {
                movsd_load(c, 15, RIP, constant(c, UINT64_C(0x7FFFFFFFFFFFFFFF)));
                sse_rr(c, 0x66, 0x54, 0, 15);
            }
            finite_or(c, fallback);
            return;
    }
}

static JitCall *new_call(Compiler *c, Node *node) {
    JitCall *site = calloc(1, sizeof *site);
    if (!site) return reject(c, node, "out of memory"), NULL;
    site->node = node;
    site->next = c->calls;
    c->calls = site;
    return site;
}

static CtType call(Compiler *c, Node *node) {
    Symbol *symbol;
    Node *function;
    int direct = direct_call(c, node, &symbol, &function);
    CtType result = call_type(c, node);
    if (c->failed) return CT_VOID;
    size_t count = 0;
    for (Node *a = node->right; a; a = a->next) ++count;
    if (count > 16) return reject(c, node, "too many arguments"), CT_VOID;
    int temps[16], callee_temp = 0;
    CtType types[16], pointer = CT_VOID;
    Node *parameter = function ? function->left : NULL;
    if (function) {
        size_t parameters = 0;
        for (Node *p = function->left; p; p = p->next) ++parameters;
        if (parameters != count || function->variadic) return reject(c, node, "argument count"), CT_VOID;
    }
    if (!direct) {
        pointer = gen(c, pointer_callee(c, node->left));
        callee_temp = temp_push(c);
        spill(c, pointer, callee_temp);
    }
    size_t index = 0;
    for (Node *a = node->right; a; a = a->next, ++index) {
        CtType type = gen(c, a);
        if (c->failed) return CT_VOID;
        if (parameter) {
            convert(c, type, parameter->type, 0, parameter, RAX, 0);
            type = parameter->type;
            parameter = parameter->next;
        } else if (type == CT_VOID) return reject(c, a, "void argument"), CT_VOID;
        types[index] = type;
        temps[index] = temp_push(c);
        if (!function || a->next) spill(c, type, temps[index]);
    }
    if (function) {
        JitCode *callee = code_for(c->interpreter, function);
        if (!callee) return reject(c, node, "callee cannot be called natively"), CT_VOID;
        static const int integer_registers[] = {RDI, RSI, RDX, RCX, R8, R9};
        int integers = 0, reals = 0;
        for (size_t i = 0; i < count; ++i) {
            if (is_real(types[i])) ++reals;
            else ++integers;
        }
        for (size_t i = count; i-- > 0;) {
            int last = i + 1 == count;
            if (is_real(types[i])) {
                --reals;
                if (last) movapd(c, reals, 0);
                else movsd_load(c, reals, RBP, temps[i]);
            } else {
                --integers;
                if (last) mov_rr(c, integer_registers[integers], RAX);
                else load64(c, integer_registers[integers], RBP, temps[i]);
            }
        }
        if (function == c->function) {
            emit(c, 0xE8);
            fixup(c, c->entry_label, 0);
        } else {
            mov_pointer(c, RAX, &callee->entry);
            op_rm(c, 0, 0, 0xFF, 2, RAX, 0, 0);
        }
    } else if (direct) {
        size_t builtin;
        if (!builtin_resolve(node->left->token, &builtin)) return reject(c, node, "unknown library function"), CT_VOID;
        JitCall *site = new_call(c, node);
        if (!site) return CT_VOID;
        assume_library(c, node->left);
        site->prototype = symbol ? symbol->function : NULL;
        site->builtin = builtin;
        site->count = count;
        site->result = result;
        size_t which = 0;
        int kind = count == 1 ? intrinsic_kind(node, types[0], result, &which) : INTRINSIC_NONE;
        int fallback = new_label(c), done = new_label(c), saved = 0;
        if (kind) {
            intrinsic(c, kind, which, types[0], temps[0], fallback);
            bind(c, done);
            open_cold(c, fallback, &saved);
        }
        outgoing_values(c, count, temps, types);
        mov_rr(c, RDX, RSP);
        mov_pointer(c, RSI, site);
        mov_rr(c, RDI, R15);
        call_interpreter(c, (Helper)jit_call_library);
        finish_value(c, result);
        if (kind) close_cold(c, done, saved);
    } else {
        JitCall *site = new_call(c, node);
        if (!site) return CT_VOID;
        site->count = count;
        site->result = result;
        site->pointer = pointer;
        outgoing_values(c, count, temps, types);
        mov_rr(c, RCX, RSP);
        load64(c, RDX, RBP, callee_temp);
        mov_pointer(c, RSI, site);
        mov_rr(c, RDI, R15);
        call_interpreter(c, (Helper)jit_call_pointer);
        finish_value(c, result);
    }
    for (size_t i = 0; i < count; ++i) temp_pop(c);
    if (!direct) temp_pop(c);
    return result;
}

static CtType gen_name(Compiler *c, Node *node) {
    Name name;
    if (!resolve_name(c, node, &name)) return CT_VOID;
    switch (name.kind) {
        case NAME_CONSTANT:
            mov_imm(c, RAX, name.constant.as.unsigned_integer);
            return name.type;
        case NAME_LIBRARY:
            assume_library(c, node);
            mov_rr(c, RDI, R15);
            mov_pointer(c, RSI, node);
            call_helper(c, (Helper)jit_library_value);
            return name.type;
        case NAME_FUNCTION:
            if (!name.symbol) return reject(c, node, "library function as a value"), CT_VOID;
            mov_imm(c, RAX, name.symbol->address);
            return type_pointer(name.type);
        case NAME_GLOBAL:
            if (type_is_array(name.type) || type_is_aggregate(name.type)) {
                mov_imm(c, RAX, name.symbol->address);
                return value_type(c, node, name.type);
            }
            mov_pointer(c, RCX, name.symbol->object->data);
            load_typed(c, name.type, is_real(name.type) ? 0 : RAX, RCX, 0);
            return name.type;
        default: break;
    }
    Local *local = name.local;
    if (local->storage == LOCAL_SLOT) {
        read_local(c, local, node, RAX, 0);
        return local->type;
    }
    if (local->storage == LOCAL_STATIC) static_address(c, local);
    else load64(c, RAX, RBP, local->offset);
    if (type_is_array(local->type) || type_is_aggregate(local->type)) return value_type(c, node, local->type);
    JitAccess *access = new_access(c, node, type_pointer(local->type), CT_INT, local->type, type_size(local->type));
    if (access) access_load(c, access);
    return local->type;
}

/* An array's name always yields the start of its own object. */
static int base_check(Compiler *c, Node *pointer) {
    Name name;
    if (pointer->kind != N_NAME || !resolve_name(c, pointer, &name)) return POINTER_INSIDE;
    return (name.kind == NAME_LOCAL || name.kind == NAME_GLOBAL) && type_is_array(name.type) ? POINTER_AT_START : POINTER_INSIDE;
}


static int needs_flag(const Local *local) {
    return local->storage == LOCAL_SLOT && !local->incoming && !local->declaration->left;
}

static int names(const Node *node, const Local *local) {
    return node && node->kind == N_NAME && node->resolution == RESOLVE_LOCAL &&
           node->cache.use.declaration == local->declaration;
}

static int changes_list(const Node *node, const Local *local);

static int changes(const Node *node, const Local *local) {
    if (!node) return 0;
    if (node == local->declaration) return 1;
    int op = node->token.kind;
    if (node->kind == N_BINARY && is_assignment(op) && names(node->left, local)) return 1;
    if ((node->kind == N_UNARY || node->kind == N_POSTFIX) && (op == TK_INCREMENT || op == TK_DECREMENT) &&
        names(node->left, local))
        return 1;
    return changes_list(node->left, local) || changes_list(node->right, local) || changes_list(node->third, local) ||
           changes_list(node->fourth, local);
}

static int changes_list(const Node *node, const Local *local) {
    for (; node; node = node->next)
        if (changes(node, local)) return 1;
    return 0;
}

static int calls_out_node(const Node *node) {
    return node && (node->kind == N_CALL || calls_out(node->left) || calls_out(node->right) || calls_out(node->third) ||
                    calls_out(node->fourth));
}

static int loop_calls_out(const Node *loop) {
    return calls_out_node(loop->left) || calls_out_node(loop->right) || calls_out_node(loop->third) ||
           calls_out_node(loop->fourth);
}

static int loop_changes(const Node *loop, const Local *local) {
    return changes(loop->left, local) || changes(loop->right, local) || changes(loop->third, local) ||
           changes(loop->fourth, local);
}

static View *find_view(Compiler *c, Node *pointer) {
    if (pointer->kind != N_NAME) return NULL;
    Name name;
    if (!resolve_name(c, pointer, &name)) return NULL;
    for (size_t i = c->view_count; i-- > 0;) {
        View *view = &c->views[i];
        if ((name.kind == NAME_LOCAL && view->local == name.local) || (name.kind == NAME_GLOBAL && view->symbol == name.symbol))
            return view;
    }
    return NULL;
}

/* Whether indexing through the name inside the loop may use a view. */
static int view_candidate(Compiler *c, Node *loop, Node *pointer, View *view) {
    Name name;
    *view = (View){0};
    if (pointer->kind != N_NAME || !resolve_name(c, pointer, &name)) return 0;
    CtType element;
    if (name.kind == NAME_LOCAL) {
        Local *local = name.local;
        if (local->storage == LOCAL_SLOT && type_is_pointer(local->type) && !needs_flag(local) && !loop_changes(loop, local))
            element = type_target(local->type);
        else if (local->storage != LOCAL_SLOT && type_is_array(local->type) && !loop_changes(loop, local))
            element = type_target(local->type);
        else return 0;
        view->local = local;
    } else if (name.kind == NAME_GLOBAL && type_is_array(name.type)) {
        element = type_target(name.type);
        view->symbol = name.symbol;
    } else return 0;
    if (!supported(element) || !type_size(element)) return 0;
    view->element = element;
    view->width = type_size(element);
    return 1;
}

static void gather_views(Compiler *c, Node *loop, Node *node, View *found, size_t *count) {
    for (; node && *count < MAX_VIEWS; node = node->next) {
        if (node->kind == N_INDEX) {
            View view;
            if (view_candidate(c, loop, node->left, &view)) {
                int known = 0;
                for (size_t i = 0; i < *count; ++i)
                    known |= found[i].local == view.local && found[i].symbol == view.symbol;
                if (!known) found[(*count)++] = view;
            }
        }
        gather_views(c, loop, node->left, found, count);
        gather_views(c, loop, node->right, found, count);
        gather_views(c, loop, node->third, found, count);
        gather_views(c, loop, node->fourth, found, count);
    }
}

static size_t loop_views(Compiler *c, Node *loop, View *found) {
    size_t count = 0;
    if (loop_calls_out(loop)) return 0;
    gather_views(c, loop, loop->left, found, &count);
    gather_views(c, loop, loop->right, found, &count);
    gather_views(c, loop, loop->third, found, &count);
    gather_views(c, loop, loop->fourth, found, &count);
    return count;
}

static void survey_views(Compiler *c, Node *node, size_t *total) {
    for (; node; node = node->next) {
        if (node->kind == N_WHILE || node->kind == N_DO || node->kind == N_FOR) {
            View found[MAX_VIEWS];
            *total += loop_views(c, node, found);
        }
        survey_views(c, node->left, total);
        survey_views(c, node->right, total);
        survey_views(c, node->third, total);
        survey_views(c, node->fourth, total);
    }
}

static void view_pointer(Compiler *c, View *view) {
    if (view->symbol) mov_imm(c, RAX, view->symbol->address);
    else if (view->local->storage == LOCAL_SLOT) read_local(c, view->local, NULL, RAX, 0);
    else if (view->local->storage == LOCAL_STATIC) static_address(c, view->local);
    else load64(c, RAX, RBP, view->local->offset);
}

static void setup_view(Compiler *c, View *view) {
    int slow = new_label(c), done = new_label(c), saved;
    view_pointer(c, view);
    mov_pointer(c, RDX, view->site);
    load64(c, R8, R15, EPOCH);
    alu_rm(c, ALU_CMP, 1, R8, RDX, SITE(epoch));
    jcc(c, CC_NE, slow);
    alu_mi(c, ALU_CMP, 1, RDX, SITE(writable), 0);
    jcc(c, CC_E, slow);
    mov_rr(c, RCX, RAX);
    alu_rm(c, ALU_SUB, 1, RCX, RDX, SITE(base));
    alu_rm(c, ALU_CMP, 1, RCX, RDX, SITE(size));
    jcc(c, CC_A, slow);
    if (view->width > 1) {
        emit(c, 0xA8);
        emit(c, (unsigned)(view->width - 1));
        jcc(c, CC_NE, slow);
    }
    store64(c, RBP, view->slots, RCX);
    store64(c, RBP, view->slots + 8, RCX);
    load64(c, R8, RDX, SITE(limit));
    store64(c, RBP, view->slots + 16, R8);
    load64(c, R8, RDX, SITE(data));
    store64(c, RBP, view->slots + 24, R8);
    load64(c, R8, RDX, SITE(initialized));
    store64(c, RBP, view->slots + 32, R8);
    bind(c, done);
    open_cold(c, slow, &saved);
    lea(c, RCX, RBP, view->slots);
    mov_rr(c, RDX, RAX);
    mov_pointer(c, RSI, view->site);
    mov_rr(c, RDI, R15);
    call_helper(c, (Helper)jit_view);
    close_cold(c, done, saved);
}

static size_t open_views(Compiler *c, Node *loop) {
    size_t before = c->view_count;
    View found[MAX_VIEWS];
    size_t count = loop_views(c, loop, found);
    for (size_t i = 0; i < count && c->view_count < c->view_groups; ++i) {
        int active = 0;
        for (size_t j = 0; j < c->view_count; ++j)
            active |= c->views[j].local == found[i].local && c->views[j].symbol == found[i].symbol;
        if (active) continue;
        View *view = &c->views[c->view_count];
        *view = found[i];
        view->slots = c->view_base - VIEW_SLOT_BYTES * (int)(c->view_count + 1);
        view->site = new_access(c, loop, type_pointer(view->element), CT_INT, view->element, view->width);
        if (!view->site) return before;
        ++c->view_count;
        setup_view(c, view);
    }
    return before;
}

/* rcx: the index, of the given type -> rcx: the element's host address,
 * rdi: the index; jumps to miss unless the view vouches for the element. */
static void view_element(Compiler *c, View *view, Node *index_node, CtType index, int store, int miss) {
    if (!simple(c, index_node)) mov_rr(c, RDI, RCX);
    if (wide(index)) {
        op_rr(c, 0, 1, 0x63, R8, RCX, 0);
        alu_rr(c, ALU_CMP, 1, R8, RCX);
        jcc(c, CC_NE, miss);
    }
    if (view->width > 1) {
        if (power_of_two((int64_t)view->width)) shift_immediate(c, 4, RCX, log2_of((int64_t)view->width));
        else imul_ri(c, 1, RCX, RCX, (int32_t)view->width);
    }
    alu_rm(c, ALU_ADD, 1, RCX, RBP, view->slots + (store ? 8 : 0));
    alu_rm(c, ALU_CMP, 1, RCX, RBP, view->slots + 16);
    jcc(c, CC_A, miss);
    int initialized = new_label(c), flags = new_label(c), saved;
    alu_mi(c, ALU_CMP, 1, RBP, view->slots + 32, 0);
    jcc(c, CC_NE, flags);
    bind(c, initialized);
    alu_rm(c, ALU_ADD, 1, RCX, RBP, view->slots + 24);
    open_cold(c, flags, &saved);
    load64(c, R8, RBP, view->slots + 32);
    alu_rr(c, ALU_ADD, 1, R8, RCX);
    switch (view->width) {
        case 1: op_rm(c, 0, 0, 0x80, 7, R8, 0, 0); emit(c, 1); break;
        case 2: op_rm(c, 0x66, 0, 0x81, 7, R8, 0, 0); emit(c, 1); emit(c, 1); break;
        case 4: op_rm(c, 0, 0, 0x81, 7, R8, 0, 0); emit32(c, 0x01010101u); break;
        default:
            mov_imm(c, R9, UINT64_C(0x0101010101010101));
            op_rm(c, 0, 1, 0x39, R9, R8, 0, 0);
            break;
    }
    jcc(c, CC_NE, miss);
    close_cold(c, initialized, saved);
}

/* rcx: the index again, on a miss path after view_element. */
static void recover_index(Compiler *c, Node *index_node) {
    if (simple(c, index_node)) gen_simple(c, index_node, RCX, 1);
    else mov_rr(c, RCX, RDI);
}

static int gen_index_into_rcx(Compiler *c, Node *index_node, CtType index) {
    if (simple(c, index_node)) {
        gen_simple(c, index_node, RCX, 1);
        return 1;
    }
    if (gen(c, index_node) != index) return reject(c, index_node, "inconsistent type");
    mov_rr(c, RCX, RAX);
    return 1;
}

static CtType gen_index(Compiler *c, Node *node) {
    CtType pointer = type_of(c, node->left), index = type_of(c, node->right);
    if (c->failed) return CT_VOID;
    if (!type_is_pointer(pointer) || !type_is_integer(index)) return reject(c, node, "unsupported index"), CT_VOID;
    CtType element = type_target(pointer);
    size_t size = type_size(element);
    int array = type_is_array(element) || type_is_aggregate(element);
    if (!size || (!array && !supported(element))) return reject(c, node, "unsupported element"), CT_VOID;
    if (gen(c, node->left) != pointer) return reject(c, node, "inconsistent type"), CT_VOID;
    if (simple(c, node->right)) gen_simple(c, node->right, RCX, 1);
    else {
        int saved = temp_push(c);
        spill(c, pointer, saved);
        if (gen(c, node->right) != index) return reject(c, node, "inconsistent type"), CT_VOID;
        mov_rr(c, RCX, RAX);
        reload(c, pointer, saved, RAX, 0);
        temp_pop(c);
    }
    CtType result = type_is_array(element) ? type_pointer(type_target(element)) : element;
    JitAccess *access = new_access(c, node, pointer, index, element, array ? 0 : size);
    if (access) element_load(c, access, size, result, base_check(c, node->left));
    return result;
}

static CtType gen_view_index(Compiler *c, Node *node, View *view) {
    CtType pointer = type_of(c, node->left), index = type_of(c, node->right);
    if (c->failed) return CT_VOID;
    if (!type_is_integer(index)) return reject(c, node, "unsupported index"), CT_VOID;
    if (!gen_index_into_rcx(c, node->right, index)) return CT_VOID;
    int miss = new_label(c), done = new_label(c), saved;
    view_element(c, view, node->right, index, 0, miss);
    load_typed(c, view->element, is_real(view->element) ? 0 : RAX, RCX, 0);
    bind(c, done);
    open_cold(c, miss, &saved);
    view_pointer(c, view);
    recover_index(c, node->right);
    JitAccess *access = new_access(c, node, pointer, index, view->element, view->width);
    if (access) element_load(c, access, view->width, view->element, base_check(c, node->left));
    close_cold(c, done, saved);
    return view->element;
}

static CtType gen_unary(Compiler *c, Node *node) {
    int op = node->token.kind;
    if (op == TK_INCREMENT || op == TK_DECREMENT) return step(c, node);
    if (op == '&') {
        CtType object;
        if (!object_address(c, node->left, &object)) return CT_VOID;
        return type_pointer(object);
    }
    if (op == '*') {
        CtType pointer = gen(c, node->left);
        if (!type_is_pointer(pointer)) return reject(c, node, "dereference of a non-pointer"), CT_VOID;
        CtType target = type_target(pointer);
        if (type_is_array(target) || type_is_aggregate(target)) return value_type(c, node, target);
        if (!supported(target)) return reject(c, node, "unsupported dereference"), CT_VOID;
        JitAccess *access = new_access(c, node, pointer, CT_INT, target, type_size(target));
        if (access) access_load(c, access);
        return target;
    }
    if (op == '!') {
        int yes = new_label(c), done = new_label(c);
        branch(c, node->left, 0, yes);
        mov_imm(c, RAX, 0);
        jmp(c, done);
        bind(c, yes);
        mov_imm(c, RAX, 1);
        bind(c, done);
        return CT_INT;
    }
    CtType type = gen(c, node->left);
    if (!arithmetic(type)) return reject(c, node, "unary operator on a non-number"), CT_VOID;
    CtType promoted = type_promote(type);
    convert(c, type, promoted, 0, node, RAX, 0);
    if (op == '+') return promoted;
    if (is_real(promoted)) {
        if (op != '-') return reject(c, node, "operator requires an integer operand"), CT_VOID;
        movq_from_xmm(c, RAX, 0);
        mov_imm(c, RCX, UINT64_C(1) << 63);
        alu_rr(c, ALU_XOR, 1, RAX, RCX);
        movq_to_xmm(c, 0, RAX);
        return promoted;
    }
    int w = wide(promoted);
    if (op == '~') {
        group3(c, 2, w, RAX);
        if (!w) canonical(c, promoted, RAX);
        return promoted;
    }
    group3(c, 3, w, RAX);
    if (type_is_signed(promoted)) jcc(c, CC_O, cold_label(c, node, FAIL_OVERFLOW));
    if (!w) canonical(c, promoted, RAX);
    return promoted;
}

static CtType gen_logical(Compiler *c, Node *node) {
    int yes = new_label(c), done = new_label(c);
    branch(c, node, 1, yes);
    mov_imm(c, RAX, 0);
    jmp(c, done);
    bind(c, yes);
    mov_imm(c, RAX, 1);
    bind(c, done);
    return CT_INT;
}

static CtType gen_conditional(Compiler *c, Node *node) {
    CtType type = type_of(c, node);
    int otherwise = new_label(c), done = new_label(c);
    branch(c, node->left, 0, otherwise);
    CtType first = gen(c, node->right);
    convert(c, first, type, 0, node, RAX, 0);
    jmp(c, done);
    bind(c, otherwise);
    CtType second = gen(c, node->third);
    convert(c, second, type, 0, node, RAX, 0);
    bind(c, done);
    return type;
}

static CtType gen_size(Compiler *c, Node *node) {
    CtType type = node->left ? object_type(c, node->left) : node->type;
    if (c->failed) return CT_VOID;
    const TypeInfo *info = type_info(type);
    size_t size = node->kind == N_SIZEOF ? info->size : info->align;
    if (!size || !info->complete) return reject(c, node, "incomplete sizeof"), CT_VOID;
    mov_imm(c, RAX, size);
    return CT_ULONG;
}

static CtType gen(Compiler *c, Node *node) {
    if (c->failed) return CT_VOID;
    switch (node->kind) {
        case N_VALUE: {
            if (!supported(node->token.value.type)) return reject(c, node, "unsupported constant"), CT_VOID;
            return gen_simple(c, node, RAX, 0);
        }
        case N_STRING: {
            int slow = new_label(c), done = new_label(c), saved;
            mov_pointer(c, RAX, &node->address);
            load64(c, RAX, RAX, 0);
            test_rr(c, 1, RAX, RAX);
            jcc(c, CC_E, slow);
            bind(c, done);
            open_cold(c, slow, &saved);
            mov_rr(c, RDI, R15);
            mov_pointer(c, RSI, node);
            call_helper(c, (Helper)jit_string);
            close_cold(c, done, saved);
            return type_pointer(CT_CHAR);
        }
        case N_NAME: return gen_name(c, node);
        case N_CAST: {
            CtType type = gen(c, node->left);
            if (node->type != CT_VOID && !supported(node->type)) return reject(c, node, "unsupported cast"), CT_VOID;
            convert(c, type, node->type, 1, node, RAX, 0);
            return node->type;
        }
        case N_SIZEOF: case N_ALIGNOF: return gen_size(c, node);
        case N_COMMA:
            (void)gen(c, node->left);
            return gen(c, node->right);
        case N_CALL: return call(c, node);
        case N_INDEX: {
            View *view = find_view(c, node->left);
            return view ? gen_view_index(c, node, view) : gen_index(c, node);
        }
        case N_MEMBER: {
            CtType object;
            if (!object_address(c, node, &object)) return CT_VOID;
            if (type_is_array(object) || type_is_aggregate(object)) return value_type(c, node, object);
            if (!supported(object)) return reject(c, node, "unsupported member"), CT_VOID;
            JitAccess *access = new_access(c, node, type_pointer(object), CT_INT, object, type_size(object));
            if (access) access_load(c, access);
            return object;
        }
        case N_UNARY: case N_POSTFIX: return gen_unary(c, node);
        case N_CONDITIONAL: return gen_conditional(c, node);
        case N_BINARY: {
            int op = node->token.kind;
            if (is_assignment(op)) return assign(c, node);
            if (op == TK_AND || op == TK_OR) return gen_logical(c, node);
            CtType left = gen(c, node->left);
            return operate(c, op, node, left, -1, node->right, 0, -1);
        }
        default: return reject(c, node, "unsupported expression"), CT_VOID;
    }
}

/* Jumps to label when the truth of node equals when. */
static void branch(Compiler *c, Node *node, int when, int label) {
    if (c->failed) return;
    if (node->kind == N_VALUE && supported(node->token.value.type)) {
        CtValue value = node->token.value;
        int truth = is_real(value.type) ? value.as.real != 0.0 : value.as.integer != 0;
        if (truth == when) jmp(c, label);
        return;
    }
    if (node->kind == N_UNARY && node->token.kind == '!') {
        branch(c, node->left, !when, label);
        return;
    }
    if (node->kind == N_BINARY && (node->token.kind == TK_AND || node->token.kind == TK_OR)) {
        int and = node->token.kind == TK_AND;
        if (and != when) {
            branch(c, node->left, when, label);
            branch(c, node->right, when, label);
        } else {
            int skip = new_label(c);
            branch(c, node->left, !when, skip);
            branch(c, node->right, when, label);
            bind(c, skip);
        }
        return;
    }
    int64_t divisor;
    Node *remainder = node->left;
    if (node->kind == N_BINARY && (node->token.kind == TK_EQ || node->token.kind == TK_NE) &&
        node->right->kind == N_VALUE && type_is_integer(node->right->token.value.type) && !node->right->token.value.as.integer &&
        remainder->kind == N_BINARY && remainder->token.kind == '%' &&
        type_is_integer(type_of(c, remainder->left)) && type_is_integer(type_of(c, remainder->right)) &&
        immediate_of(remainder->right, type_common(type_of(c, remainder->left), type_of(c, remainder->right)), &divisor) &&
        power_of_two(divisor) && divisor > 1) {
        CtType left = gen(c, remainder->left);
        convert(c, left, type_common(left, type_of(c, remainder->right)), 0, remainder, RAX, 0);
        alu_ri(c, ALU_AND, 1, RAX, (int32_t)(divisor - 1));
        jcc(c, (node->token.kind == TK_EQ) == when ? CC_E : CC_NE, label);
        return;
    }
    Local *direct = node->kind == N_BINARY && comparison(node->token.kind) ? register_local(c, node->left) : NULL;
    if (direct) {
        CtType right_type = type_of(c, node->right);
        Local *other = register_local(c, node->right);
        int64_t immediate = 0;
        CtType common = type_is_integer(right_type) && type_is_integer(direct->type) ? type_common(direct->type, right_type)
                      : type_is_pointer(direct->type) && right_type == direct->type &&
                        (node->token.kind == TK_EQ || node->token.kind == TK_NE) ? direct->type : CT_VOID;
        int fits = common != CT_VOID && (type_is_pointer(common) || integer_fits(direct->type, common));
        int right_ok = other ? (type_is_pointer(common) || integer_fits(other->type, common))
                     : type_is_integer(common) && immediate_of(node->right, common, &immediate);
        if (!c->failed && fits && right_ok) {
            if (other) alu_rr(c, ALU_CMP, 1, direct->reg, other->reg);
            else alu_ri(c, ALU_CMP, 1, direct->reg, (int32_t)immediate);
            int cc = condition_code(node->token.kind, type_is_integer(common) && type_is_signed(common), 0);
            jcc(c, when ? cc : cc ^ 1, label);
            return;
        }
    }
    if (node->kind == N_BINARY && comparison(node->token.kind)) {
        CtType left = gen(c, node->left);
        (void)operate(c, node->token.kind, node, left, -1, node->right, when, label);
        return;
    }
    CtType type = gen(c, node);
    if (c->failed) return;
    if (type == CT_VOID || !supported(type)) { (void)reject(c, node, "condition is not a scalar"); return; }
    if (is_real(type)) {
        sse_rr(c, 0x66, 0x57, 15, 15);
        ucomisd(c, 0, 15);
    } else test_rr(c, 1, RAX, RAX);
    jcc(c, when ? CC_NE : CC_E, label);
}

static size_t weight(const Node *node);

static size_t list_weight(const Node *node) {
    size_t total = 0;
    for (; node; node = node->next) total += weight(node);
    return total;
}

static size_t lesser(size_t a, size_t b) { return a < b ? a : b; }

/* At most how many steps the interpreter charges for a node along its
 * cheapest path, not counting the iterations of loops inside it, which charge
 * for themselves. Undercounting only lets a runaway program run a little
 * longer; overcounting would stop programs the interpreter finishes. */
static size_t weight(const Node *node) {
    if (!node) return 0;
    switch (node->kind) {
        case N_VALUE: case N_NAME: case N_CASE: return 0;
        case N_WHILE: case N_DO: return 1;
        case N_FOR: return 2 + list_weight(node->left);
        case N_IF: case N_CONDITIONAL:
            return 1 + list_weight(node->left) + lesser(list_weight(node->right), list_weight(node->third));
        case N_SWITCH: return 2 + list_weight(node->left);
        case N_BINARY:
            if (node->token.kind == TK_AND || node->token.kind == TK_OR) return 1 + list_weight(node->left);
            return 1 + list_weight(node->left) + list_weight(node->right);
        default:
            return 1 + list_weight(node->left) + list_weight(node->right) + list_weight(node->third) +
                   list_weight(node->fourth);
    }
}

static void statement(Compiler *c, Node *node);

static void release_from(Compiler *c, size_t live) {
    for (size_t i = c->live_count; i-- > live;) {
        Local *local = &c->locals[c->live[i]];
        mov_rr(c, RDI, R15);
        load64(c, RSI, RBP, local->offset);
        call_helper(c, (Helper)jit_release);
        alu_mi(c, ALU_AND, 1, RBP, local->offset, 0);
    }
}

static void close_scope(Compiler *c, size_t live) {
    release_from(c, live);
    c->live_count = live;
}

static void make_live(Compiler *c, Local *local) {
    if (c->live_count == c->live_capacity) {
        size_t capacity = c->live_capacity ? 2 * c->live_capacity : 16;
        size_t *grown = realloc(c->live, capacity * sizeof *grown);
        if (!grown) { (void)reject(c, local->declaration, "out of memory"); return; }
        c->live = grown;
        c->live_capacity = capacity;
    }
    c->live[c->live_count++] = (size_t)(local - c->locals);
}

/* Whether the interpreter can evaluate it on native code's behalf: it names no local. */
static int interpreter_safe(const Node *node) {
    for (; node; node = node->next) {
        if (node->kind == N_NAME && node->resolution != RESOLVE_GLOBAL) return 0;
        if (node->kind == N_COMPOUND || node->kind == N_VA_ARG || node->kind == N_VA_START ||
            node->kind == N_VA_END || node->kind == N_VA_COPY || node->kind == N_GENERIC)
            return 0;
        if (!interpreter_safe(node->left) || !interpreter_safe(node->right) || !interpreter_safe(node->third) ||
            !interpreter_safe(node->fourth))
            return 0;
    }
    return 1;
}

static int initialized_by_interpreter(const Local *local) {
    Node *initializer = local->declaration->left;
    return initializer && (initializer->kind == N_INITIALIZER || !supported(local->type));
}

static void declaration(Compiler *c, Node *node) {
    Local *local = find_local(c, node);
    if (!local) { (void)reject(c, node, "unknown declaration"); return; }
    Node *initializer = node->left;
    if (initializer && initializer->kind == N_INITIALIZER && local->storage == LOCAL_SLOT) {
        (void)reject(c, node, "initializer list");
        return;
    }
    if (local->storage == LOCAL_STATIC) {
        int done = new_label(c), slow = new_label(c), saved;
        static_address(c, local);
        test_rr(c, 1, RAX, RAX);
        jcc(c, CC_E, slow);
        bind(c, done);
        open_cold(c, slow, &saved);
        mov_rr(c, RDI, R15);
        mov_pointer(c, RSI, node);
        call_interpreter(c, (Helper)jit_static);
        close_cold(c, done, saved);
        return;
    }
    if (local->storage == LOCAL_SLOT) {
        if (!initializer) {
            if (local->flag) {
                op_rm(c, 0, 0, 0xC6, 0, RBP, local->flag, 0);
                emit(c, 0);
            }
            return;
        }
        gen_as(c, initializer, local->type, initializer);
        write_local(c, local, RAX, 0);
        return;
    }
    mov_rr(c, RDI, R15);
    mov_pointer(c, RSI, node);
    call_helper(c, (Helper)jit_object);
    store64(c, RBP, local->offset, RAX);
    make_live(c, local);
    if (initializer && type_is_aggregate(local->type) && initializer->kind != N_INITIALIZER && !interpreter_safe(initializer)) {
        int destination = temp_push(c);
        store64(c, RBP, destination, RAX);
        if (gen(c, initializer) != local->type) { (void)reject(c, node, "aggregate initializer of another type"); return; }
        copy_aggregate(c, initializer, local->type, destination);
        temp_pop(c);
    } else if (initialized_by_interpreter(local)) {
        if (!interpreter_safe(initializer)) { (void)reject(c, node, "initializer reads locals"); return; }
        mov_rr(c, RDX, RAX);
        mov_pointer(c, RSI, node);
        mov_rr(c, RDI, R15);
        call_interpreter(c, (Helper)jit_initialize);
    } else if (initializer) {
        CtType type = gen(c, initializer);
        convert(c, type, local->type, 0, initializer, RAX, 0);
        if (is_real(local->type)) movapd(c, 1, 0);
        else mov_rr(c, R10, RAX);
        load64(c, RAX, RBP, local->offset);
        JitAccess *access = new_access(c, initializer, type_pointer(local->type), CT_INT, local->type, type_size(local->type));
        if (access) access_store(c, access);
    }
    if (node->is_const) {
        mov_rr(c, RDI, R15);
        load64(c, RSI, RBP, local->offset);
        call_helper(c, (Helper)jit_protect);
    }
}

static void scoped(Compiler *c, Node *node) {
    size_t live = c->live_count;
    statement(c, node);
    close_scope(c, live);
}

static void sequence(Compiler *c, Node *head) {
    for (Node *node = head; node && !c->failed; node = node->next) statement(c, node);
}

static Loop *push_loop(Compiler *c, Node *node, int break_label, int continue_label) {
    if (c->loop_count == sizeof c->loops / sizeof c->loops[0]) return reject(c, node, "loops nested too deeply"), NULL;
    Loop *loop = &c->loops[c->loop_count++];
    *loop = (Loop){break_label, continue_label, c->live_count};
    return loop;
}

static int constant_shaped(Compiler *c, Node *node) {
    switch (node->kind) {
        case N_VALUE: return type_is_integer(node->token.value.type);
        case N_NAME: {
            Name name;
            return node->resolution == RESOLVE_GLOBAL && resolve_name(c, node, &name) && name.kind == NAME_CONSTANT;
        }
        case N_UNARY:
            return (node->token.kind == '-' || node->token.kind == '+' || node->token.kind == '~' ||
                    node->token.kind == '!') && constant_shaped(c, node->left);
        case N_CAST: return type_is_integer(node->type) && constant_shaped(c, node->left);
        case N_BINARY:
            return !is_assignment(node->token.kind) && constant_shaped(c, node->left) && constant_shaped(c, node->right);
        default: return 0;
    }
}

static int case_value(Compiler *c, Node *item, int64_t *value) {
    CtValue folded;
    if (!constant_shaped(c, item->left) || !runtime_fold(c->interpreter, item->left, &folded) ||
        !type_is_integer(folded.type))
        return reject(c, item, "case label is not a simple constant");
    *value = folded.as.integer;
    return 1;
}

static void switch_statement(Compiler *c, Node *node) {
    CtType type = type_of(c, node->left);
    if (c->failed) return;
    if (!type_is_integer(type) || !node->right || node->right->kind != N_BLOCK) {
        (void)reject(c, node, "unsupported switch");
        return;
    }
    size_t count = 0;
    for (Node *item = node->right->left; item; item = item->next) {
        if (item->kind == N_DECLARATION || item->kind == N_GROUP) { (void)reject(c, item, "declaration in a switch"); return; }
        if (item->kind == N_CASE && item->left) ++count;
    }
    int64_t *values = count ? malloc(count * sizeof *values) : NULL;
    int *labels = count ? malloc(count * sizeof *labels) : NULL;
    if (count && (!values || !labels)) {
        free(values);
        free(labels);
        (void)reject(c, node, "out of memory");
        return;
    }
    int end = new_label(c), fallback = end;
    size_t index = 0;
    int64_t low = INT64_MAX, high = INT64_MIN;
    for (Node *item = node->right->left; item && !c->failed; item = item->next) {
        if (item->kind != N_CASE) continue;
        if (!item->left) {
            if (fallback != end) { (void)reject(c, item, "duplicate default"); break; }
            fallback = new_label(c);
            continue;
        }
        if (!case_value(c, item, &values[index])) break;
        for (size_t j = 0; j < index; ++j)
            if (values[j] == values[index]) (void)reject(c, item, "duplicate case");
        if (values[index] < low) low = values[index];
        if (values[index] > high) high = values[index];
        labels[index++] = new_label(c);
    }
    if (!c->failed) {
        CtType generated = gen(c, node->left);
        convert(c, generated, type_promote(type), 0, node, RAX, 0);
        int dense = count >= 4 && high - low >= 0 && (uint64_t)(high - low) < 4 * count + 8 && high - low < 4096;
        if (dense) {
            mov_rr(c, RCX, RAX);
            if (low) {
                mov_imm(c, RDX, (uint64_t)low);
                alu_rr(c, ALU_SUB, 1, RCX, RDX);
            }
            mov_imm(c, RDX, (uint64_t)(high - low));
            alu_rr(c, ALU_CMP, 1, RCX, RDX);
            jcc(c, CC_A, fallback);
            int saved = c->section;
            c->section = DATA;
            while (c->sections[DATA].length % 4) emit(c, 0);
            int table = new_label(c);
            bind(c, table);
            for (int64_t v = low; v <= high; ++v) {
                int target = fallback;
                for (size_t j = 0; j < count; ++j)
                    if (values[j] == v) target = labels[j];
                fixup(c, target, table + 1);
            }
            c->section = saved;
            op_rm(c, 0, 1, 0x8D, RDX, RIP, table, 0);
            op_sib(c, 1, 0x63, RAX, RDX, RCX, 2);
            alu_rr(c, ALU_ADD, 1, RAX, RDX);
            op_rr(c, 0, 0, 0xFF, 4, RAX, 0);
        } else {
            for (size_t j = 0; j < count; ++j) {
                if (fits32(values[j])) alu_ri(c, ALU_CMP, 1, RAX, (int32_t)values[j]);
                else {
                    mov_imm(c, RCX, (uint64_t)values[j]);
                    alu_rr(c, ALU_CMP, 1, RAX, RCX);
                }
                jcc(c, CC_E, labels[j]);
            }
            jmp(c, fallback);
        }
        Loop *loop = push_loop(c, node, end, -1);
        if (loop) {
            index = 0;
            for (Node *item = node->right->left; item && !c->failed; item = item->next) {
                if (item->kind == N_CASE) {
                    bind(c, item->left ? labels[index++] : fallback);
                    continue;
                }
                statement(c, item);
            }
            --c->loop_count;
        }
    }
    bind(c, end);
    free(values);
    free(labels);
}

static void statement(Compiler *c, Node *node) {
    if (c->failed) return;
    switch (node->kind) {
        case N_EMPTY: case N_TYPEDEF: return;
        case N_EXPRESSION: {
            c->discarding = node->terminated && node->left->kind == N_POSTFIX;
            CtType type = gen(c, node->left);
            if (node->terminated || c->failed) return;
            if (type != CT_VOID) {
                op_rm(c, 0, 0, 0xC7, 0, R15, INTERPRETER(shown), 0);
                emit32(c, (uint32_t)type);
                if (is_real(type)) movsd_store(c, R15, INTERPRETER(shown) + 8, 0);
                else store64(c, R15, INTERPRETER(shown) + 8, RAX);
            }
            op_rm(c, 0, 0, 0xC7, 0, R15, INTERPRETER(showing), 0);
            emit32(c, type != CT_VOID);
            return;
        }
        case N_GROUP: sequence(c, node->left); return;
        case N_DECLARATION:
            if (node->is_extern) { (void)reject(c, node, "block-scope extern"); return; }
            declaration(c, node);
            return;
        case N_BLOCK: {
            size_t live = c->live_count;
            sequence(c, node->left);
            close_scope(c, live);
            return;
        }
        case N_IF: {
            int otherwise = new_label(c), done = new_label(c);
            branch(c, node->left, 0, otherwise);
            if (node->right) scoped(c, node->right);
            if (node->third) jmp(c, done);
            bind(c, otherwise);
            if (node->third) scoped(c, node->third);
            bind(c, done);
            return;
        }
        case N_WHILE: case N_DO: {
            int top = new_label(c), next = new_label(c), end = new_label(c);
            size_t views = open_views(c, node);
            if (node->kind == N_WHILE) jmp(c, next);
            bind(c, top);
            if (!push_loop(c, node, end, next)) return;
            if (node->right) scoped(c, node->right);
            --c->loop_count;
            bind(c, next);
            tick(c, node, weight(node->left) + weight(node->right) + 1);
            branch(c, node->left, 1, top);
            bind(c, end);
            c->view_count = views;
            return;
        }
        case N_FOR: {
            size_t live = c->live_count;
            int top = new_label(c), next = new_label(c), test = new_label(c), end = new_label(c);
            if (node->left) statement(c, node->left);
            size_t views = open_views(c, node);
            jmp(c, test);
            bind(c, top);
            if (!push_loop(c, node, end, next)) return;
            if (node->fourth) scoped(c, node->fourth);
            --c->loop_count;
            bind(c, next);
            c->discarding = node->third && node->third->kind == N_POSTFIX;
            if (node->third) (void)gen(c, node->third);
            bind(c, test);
            tick(c, node, weight(node->right) + weight(node->third) + weight(node->fourth) + 1);
            if (node->right) branch(c, node->right, 1, top);
            else jmp(c, top);
            bind(c, end);
            c->view_count = views;
            close_scope(c, live);
            return;
        }
        case N_BREAK: case N_CONTINUE: {
            size_t i = c->loop_count;
            if (node->kind == N_CONTINUE)
                while (i > 0 && c->loops[i - 1].continue_label < 0) --i;
            if (!i) { (void)reject(c, node, "jump outside a loop"); return; }
            Loop *loop = &c->loops[i - 1];
            release_from(c, loop->live);
            jmp(c, node->kind == N_BREAK ? loop->break_label : loop->continue_label);
            return;
        }
        case N_RETURN: {
            if (node->left) {
                CtType type = gen(c, node->left);
                if (c->returns == CT_VOID) type = CT_VOID;
                else if (type == CT_VOID) { (void)reject(c, node, "void return value"); return; }
                else convert(c, type, c->returns, 0, node, RAX, 0);
            } else if (c->returns != CT_VOID) { (void)reject(c, node, "return without a value"); return; }
            if (c->live_count) {
                int value = temp_push(c);
                if (c->returns != CT_VOID) spill(c, c->returns, value);
                release_from(c, 0);
                if (c->returns != CT_VOID) reload(c, c->returns, value, RAX, 0);
                temp_pop(c);
            }
            jmp(c, c->return_label);
            return;
        }
        case N_SWITCH: switch_statement(c, node); return;
        default: (void)reject(c, node, "unsupported statement"); return;
    }
}

static void mark_addressed(Compiler *c, Node *node) {
    for (; node; node = node->next) {
        if (node->kind == N_NAME && node->resolution == RESOLVE_LOCAL && node->cache.use.declaration) {
            Local *local = find_local(c, node->cache.use.declaration);
            if (local) local->addressed = 1;
        }
        mark_addressed(c, node->left);
        mark_addressed(c, node->right);
        mark_addressed(c, node->third);
    }
}

static void survey_addresses(Compiler *c, Node *node) {
    for (; node; node = node->next) {
        if (node->kind == N_UNARY && node->token.kind == '&') mark_addressed(c, node->left);
        survey_addresses(c, node->left);
        survey_addresses(c, node->right);
        survey_addresses(c, node->third);
        survey_addresses(c, node->fourth);
    }
}

/* How much a register would save each local. A pointer that is only ever
 * indexed inside a loop is read once per loop, by a view, so its uses there
 * count as though outside it. */
static void survey_uses(Compiler *c, Node *node, double scale) {
    for (; node; node = node->next) {
        if (node->kind == N_NAME && node->resolution == RESOLVE_LOCAL && node->cache.use.declaration) {
            Local *local = find_local(c, node->cache.use.declaration);
            if (local) local->weight += scale;
        }
        double inner = node->kind == N_WHILE || node->kind == N_DO || node->kind == N_FOR ? scale * 8 : scale;
        if (node->kind == N_INDEX && node->left->kind == N_NAME) {
            survey_uses(c, node->left, inner > 1 ? inner / 8 : inner);
            survey_uses(c, node->right, inner);
            continue;
        }
        survey_uses(c, node->left, inner);
        survey_uses(c, node->right, inner);
        survey_uses(c, node->third, inner);
        survey_uses(c, node->fourth, inner);
    }
}

static void add_local(Compiler *c, Node *declaration, int parameter) {
    if (c->local_count == c->local_capacity) {
        size_t capacity = c->local_capacity ? 2 * c->local_capacity : 32;
        Local *grown = realloc(c->locals, capacity * sizeof *grown);
        if (!grown) { (void)reject(c, declaration, "out of memory"); return; }
        c->locals = grown;
        c->local_capacity = capacity;
    }
    c->locals[c->local_count++] = (Local){.declaration = declaration, .type = declaration->type, .reg = -1,
                                          .incoming = parameter, .readonly = declaration->is_const};
}

static void survey_declarations(Compiler *c, Node *node) {
    for (; node && !c->failed; node = node->next) {
        switch (node->kind) {
            case N_DECLARATION: add_local(c, node, 0); break;
            case N_ENUMERATOR: (void)reject(c, node, "local enumeration"); return;
            case N_FUNCTION: (void)reject(c, node, "nested function"); return;
            case N_BLOCK: case N_GROUP: survey_declarations(c, node->left); break;
            case N_IF: survey_declarations(c, node->right); survey_declarations(c, node->third); break;
            case N_WHILE: case N_DO: case N_SWITCH: survey_declarations(c, node->right); break;
            case N_FOR: survey_declarations(c, node->left); survey_declarations(c, node->fourth); break;
            default: break;
        }
    }
}

static int classify(Compiler *c) {
    for (size_t i = 0; i < c->local_count; ++i) {
        Local *local = &c->locals[i];
        Node *declaration = local->declaration;
        int object = type_is_array(local->type) || type_is_aggregate(local->type);
        if (object) {
            if (!type_size(local->type) || !type_info(local->type)->complete) return reject(c, declaration, "incomplete object");
        } else if (!supported(local->type)) return reject(c, declaration, "unsupported local type");
        if (declaration->is_static) {
            if (local->incoming) return reject(c, declaration, "static parameter");
            if (declaration->left && !interpreter_safe(declaration->left)) return reject(c, declaration, "static initializer");
            local->storage = LOCAL_STATIC;
        } else if (local->addressed || object) local->storage = LOCAL_MANAGED;
        else local->storage = LOCAL_SLOT;
    }
    static const int candidates[] = {RBX, R12, R13};
    for (int r = 0; r < 3; ++r) {
        Local *best = NULL;
        for (size_t i = 0; i < c->local_count; ++i) {
            Local *local = &c->locals[i];
            if (local->storage != LOCAL_SLOT || is_real(local->type) || local->reg >= 0 || local->weight <= 0) continue;
            if (!best || local->weight > best->weight) best = local;
        }
        if (!best) break;
        best->reg = candidates[r];
        c->saved[c->saved_count++] = candidates[r];
    }
    for (int r = 8; r < 14; ++r) {
        Local *best = NULL;
        for (size_t i = 0; i < c->local_count; ++i) {
            Local *local = &c->locals[i];
            if (local->storage != LOCAL_SLOT || !is_real(local->type) || local->reg >= 0 || local->weight <= 0) continue;
            if (!best || local->weight > best->weight) best = local;
        }
        if (!best) break;
        best->reg = r;
        c->xmm_locals[c->xmm_count++] = best;
    }
    size_t views = 0;
    survey_views(c, c->function->right, &views);
    c->view_groups = views < MAX_VIEWS ? views : MAX_VIEWS;
    int bytes = VIEW_SLOT_BYTES * (int)c->view_groups;
    c->view_base = -(c->saved_count * 8);
    for (size_t i = 0; i < c->local_count; ++i) {
        Local *local = &c->locals[i];
        if (local->reg < 0 || local->storage != LOCAL_SLOT || is_real(local->type)) {
            bytes += 8;
            local->offset = -(c->saved_count * 8 + bytes);
        }
        if (needs_flag(local)) {
            bytes += 8;
            local->flag = -(c->saved_count * 8 + bytes);
        }
        if (local->storage == LOCAL_MANAGED && local->incoming) {
            bytes += 8;
            local->incoming = -(c->saved_count * 8 + bytes);
        }
    }
    for (int i = 0; i < c->xmm_count; ++i) {
        bytes += 8;
        c->xmm_saves[i] = -(c->saved_count * 8 + bytes);
    }
    c->local_bytes = bytes;
    return 1;
}

static void prologue(Compiler *c) {
    push(c, RBP);
    mov_rr(c, RBP, RSP);
    for (int i = 0; i < c->saved_count; ++i) push(c, c->saved[i]);
    op_rr(c, 0, 1, 0x81, ALU_SUB, RSP, 0);
    c->frame_patch = c->sections[HOT].length;
    emit32(c, 0);
    alu_rm(c, ALU_CMP, 1, RSP, R15, INTERPRETER(jit_floor));
    jcc(c, CC_B, cold_label(c, c->function, FAIL_DEPTH));
    for (int i = 0; i < c->xmm_count; ++i) movsd_store(c, RBP, c->xmm_saves[i], c->xmm_locals[i]->reg);
    static const int integer_registers[] = {RDI, RSI, RDX, RCX, R8, R9};
    int integers = 0, reals = 0;
    for (size_t i = 0; i < c->local_count; ++i) {
        Local *local = &c->locals[i];
        if (!local->incoming) continue;
        int real = is_real(local->type);
        int source = real ? reals++ : integer_registers[integers++];
        if (local->storage == LOCAL_SLOT) {
            if (real && local->reg >= 0) movapd(c, local->reg, source);
            else if (real) movsd_store(c, RBP, local->offset, source);
            else if (local->reg >= 0) mov_rr(c, local->reg, source);
            else store64(c, RBP, local->offset, source);
        } else if (real) movsd_store(c, RBP, local->incoming, source);
        else store64(c, RBP, local->incoming, source);
    }
    for (size_t i = 0; i < c->local_count; ++i)
        if (c->locals[i].storage == LOCAL_MANAGED) alu_mi(c, ALU_AND, 1, RBP, c->locals[i].offset, 0);
    for (size_t i = 0; i < c->local_count; ++i) {
        Local *local = &c->locals[i];
        if (!local->incoming || local->storage != LOCAL_MANAGED) continue;
        mov_rr(c, RDI, R15);
        mov_pointer(c, RSI, local->declaration);
        call_helper(c, (Helper)jit_object);
        store64(c, RBP, local->offset, RAX);
        make_live(c, local);
        if (is_real(local->type)) movsd_load(c, 1, RBP, local->incoming);
        else load64(c, R10, RBP, local->incoming);
        JitAccess *access = new_access(c, local->declaration, type_pointer(local->type), CT_INT, local->type, type_size(local->type));
        if (access) access_store(c, access);
    }
}

static void epilogue(Compiler *c) {
    bind(c, c->return_label);
    for (int i = 0; i < c->xmm_count; ++i) movsd_load(c, c->xmm_locals[i]->reg, RBP, c->xmm_saves[i]);
    lea(c, RSP, RBP, -8 * c->saved_count);
    for (int i = c->saved_count; i-- > 0;) pop(c, c->saved[i]);
    pop(c, RBP);
    emit(c, 0xC3);
}

static int is_main(const Node *function) {
    return function->token.length == 4 && !memcmp(function->token.start, "main", 4);
}

static size_t layout(Compiler *c, size_t *cold, size_t *data) {
    *cold = c->sections[HOT].length;
    *data = (*cold + c->sections[COLD].length + 15) & ~(size_t)15;
    return *data + c->sections[DATA].length;
}

static size_t position(Place place, size_t cold, size_t data) {
    return place.offset + (place.section == HOT ? 0 : place.section == COLD ? cold : data);
}

static unsigned char *link_image(Compiler *c, size_t *length) {
    size_t cold, data;
    *length = layout(c, &cold, &data);
    unsigned char *image = calloc(1, *length ? *length : 1);
    if (!image) return NULL;
    memcpy(image, c->sections[HOT].bytes, c->sections[HOT].length);
    if (c->sections[COLD].length) memcpy(image + cold, c->sections[COLD].bytes, c->sections[COLD].length);
    if (c->sections[DATA].length) memcpy(image + data, c->sections[DATA].bytes, c->sections[DATA].length);
    for (size_t i = 0; i < c->fixup_count; ++i) {
        Fixup *f = &c->fixups[i];
        Place target = c->labels[f->label];
        if (target.section < 0) { free(image); return NULL; }
        size_t at = position((Place){f->section, f->at}, cold, data);
        int64_t displacement = f->table
            ? (int64_t)position(target, cold, data) - (int64_t)position(c->labels[f->table - 1], cold, data)
            : (int64_t)position(target, cold, data) - (int64_t)(at + 4);
        int32_t value = (int32_t)displacement;
        memcpy(image + at, &value, sizeof value);
    }
    return image;
}

static void compiler_free(Compiler *c) {
    for (int i = 0; i < SECTIONS; ++i) free(c->sections[i].bytes);
    free(c->labels);
    free(c->fixups);
    free(c->locals);
    free(c->live);
    free(c->assumed);
}

static int compile_body(Compiler *c) {
    Node *function = c->function;
    c->returns = type_target(function->type);
    if (c->returns != CT_VOID && !supported(c->returns)) return reject(c, function, "unsupported return type");
    if (function->variadic) return reject(c, function, "variadic function");
    for (Node *parameter = function->left; parameter; parameter = parameter->next) add_local(c, parameter, 1);
    survey_declarations(c, function->right->left);
    if (c->failed) return 0;
    survey_addresses(c, function->right);
    survey_uses(c, function->right, 1);
    if (!classify(c)) return 0;
    c->return_label = new_label(c);
    c->entry_label = new_label(c);
    bind(c, c->entry_label);
    prologue(c);
    tick(c, function, list_weight(function->right->left) + 1);
    sequence(c, function->right->left);
    if (c->failed) return 0;
    close_scope(c, 0);
    if (c->returns == CT_VOID) {}
    else if (is_main(function)) mov_imm(c, RAX, 0);
    else jmp(c, cold_label(c, function, FAIL_NO_RETURN));
    epilogue(c);
    int frame = c->saved_count * 8 + c->local_bytes + 8 * c->temp_peak + c->outgoing_peak;
    frame = (frame + 15) & ~15;
    int32_t adjust = frame - c->saved_count * 8;
    memcpy(c->sections[HOT].bytes + c->frame_patch, &adjust, sizeof adjust);
    return !c->failed;
}

static int parameter_classes(Node *function, int *integers, int *reals) {
    *integers = *reals = 0;
    for (Node *parameter = function->left; parameter; parameter = parameter->next) {
        if (!supported(parameter->type)) return 0;
        if (is_real(parameter->type)) ++*reals;
        else ++*integers;
    }
    return *integers <= 6 && *reals <= 8;
}

/* Native callers enter here until the function compiles: it forwards the
 * arguments to the interpreter, which also compiles the function on the way. */
static unsigned char *make_thunk(Node *function, size_t *mapped) {
    Compiler c = {0};
    c.function = function;
    size_t count = 0;
    for (Node *p = function->left; p; p = p->next) ++count;
    int frame = (int)((16 * count + 48 + 15) & ~(size_t)15);
    push(&c, RBP);
    mov_rr(&c, RBP, RSP);
    alu_ri(&c, ALU_SUB, 1, RSP, frame);
    for (int i = 0; i < 6; ++i) movsd_store(&c, RBP, -8 * (i + 1), 8 + i);
    static const int integer_registers[] = {RDI, RSI, RDX, RCX, R8, R9};
    int integers = 0, reals = 0, index = 0;
    for (Node *p = function->left; p; p = p->next, ++index) {
        op_rm(&c, 0, 0, 0xC7, 0, RSP, 16 * index, 0);
        emit32(&c, (uint32_t)p->type);
        if (is_real(p->type)) movsd_store(&c, RSP, 16 * index + 8, reals++);
        else store64(&c, RSP, 16 * index + 8, integer_registers[integers++]);
    }
    mov_rr(&c, RDI, R15);
    mov_pointer(&c, RSI, function);
    mov_rr(&c, RDX, RSP);
    mov_imm(&c, RCX, count);
    call_interpreter(&c, (Helper)jit_call_interpreted);
    for (int i = 0; i < 6; ++i) movsd_load(&c, 8 + i, RBP, -8 * (i + 1));
    if (is_real(type_target(function->type))) movq_to_xmm(&c, 0, RAX);
    mov_rr(&c, RSP, RBP);
    pop(&c, RBP);
    emit(&c, 0xC3);
    unsigned char *thunk = c.failed ? NULL : executable(c.sections[HOT].bytes, c.sections[HOT].length, mapped);
    compiler_free(&c);
    return thunk;
}

static JitCode *code_for(CtInterpreter *interpreter, Node *function) {
    if (function->jit) return function->jit->thunk ? function->jit : NULL;
    JitCode *code = calloc(1, sizeof *code);
    if (!code) return NULL;
    code->function = function;
    code->state = JIT_UNTRIED;
    int integers, reals;
    if (!function->right || function->variadic || !parameter_classes(function, &integers, &reals) ||
        (type_target(function->type) != CT_VOID && !supported(type_target(function->type))))
        code->state = JIT_REJECTED;
    else code->thunk = make_thunk(function, &code->thunk_size);
    if (!code->thunk) code->state = JIT_REJECTED;
    code->entry = code->thunk;
    code->next = interpreter->jit_codes;
    interpreter->jit_codes = code;
    function->jit = code;
    return code->thunk ? code : NULL;
}

static void report(Compiler *c) {
    static int checked, enabled;
    if (!checked) {
        const char *setting = getenv("CTERPRETER_JIT_LOG");
        enabled = setting && *setting && strcmp(setting, "0");
        checked = 1;
    }
    if (!enabled) return;
    const Node *node = c->reason_node ? c->reason_node : c->function;
    if (c->failed)
        fprintf(stderr, "jit: %.*s stays interpreted: %s at %zu:%zu\n", (int)c->function->token.length,
                c->function->token.start, c->reason, node->token.line, node->token.column);
    else fprintf(stderr, "jit: %.*s compiled\n", (int)c->function->token.length, c->function->token.start);
}

/* CTERPRETER_JIT_DUMP names a directory that receives each function's machine code. */
static void dump(const Node *function, const unsigned char *image, size_t length) {
    const char *directory = getenv("CTERPRETER_JIT_DUMP");
    if (!directory || !*directory || !image) return;
    char path[4096];
    int written = snprintf(path, sizeof path, "%s/%.*s.bin", directory, (int)function->token.length, function->token.start);
    if (written < 0 || (size_t)written >= sizeof path) return;
    FILE *file = fopen(path, "wb");
    if (!file) return;
    (void)fwrite(image, 1, length, file);
    fclose(file);
}

static int compile(CtInterpreter *interpreter, Node *function) {
    JitCode *code = code_for(interpreter, function);
    if (!code) return 0;
    if (code->state != JIT_UNTRIED) return code->state == JIT_COMPILED;
    code->state = JIT_COMPILING;
    Compiler c = {.interpreter = interpreter, .code = code, .function = function};
    int compiled = compile_body(&c);
    unsigned char *body = NULL;
    if (compiled) {
        size_t length;
        unsigned char *image = link_image(&c, &length);
        dump(function, image, length);
        body = image ? executable(image, length, &code->body_size) : NULL;
        free(image);
        if (!body) (void)reject(&c, function, "could not map native code");
    }
    report(&c);
    if (body) {
        code->body = body;
        code->entry = body;
        code->state = JIT_COMPILED;
        JitAccess *access = c.accesses;
        while (access) {
            JitAccess *next = access->next;
            access->next = code->accesses;
            code->accesses = access;
            access = next;
        }
        JitCall *site = c.calls;
        while (site) {
            JitCall *next = site->next;
            site->next = code->calls;
            code->calls = site;
            site = next;
        }
        code->assumed = c.assumed;
        code->assumed_count = c.assumed_count;
        c.assumed = NULL;
    } else {
        code->state = JIT_REJECTED;
        free_sites(c.accesses, c.calls);
    }
    compiler_free(&c);
    return code->state == JIT_COMPILED;
}

typedef uint64_t (*Trampoline)(CtInterpreter *interpreter, void *entry, const uint64_t *integers, double *reals);

/* Enters native code from C: pins the interpreter in r15 and passes the
 * arguments in registers; a double result is left in reals[0]. */
static Trampoline trampoline(void) {
    static Trampoline entry;
    static size_t mapped;
    if (entry) return entry;
    Compiler c = {0};
    push(&c, RBP);
    mov_rr(&c, RBP, RSP);
    push(&c, RBX);
    push(&c, R12);
    push(&c, R13);
    push(&c, R14);
    push(&c, R15);
    alu_ri(&c, ALU_SUB, 1, RSP, 8);
    mov_rr(&c, R15, RDI);
    load_budget(&c);
    mov_rr(&c, R12, RCX);
    mov_rr(&c, RAX, RSI);
    mov_rr(&c, R10, RDX);
    for (int i = 0; i < 8; ++i) movsd_load(&c, i, R12, 8 * i);
    static const int integer_registers[] = {RDI, RSI, RDX, RCX, R8, R9};
    for (int i = 0; i < 6; ++i) load64(&c, integer_registers[i], R10, 8 * i);
    op_rr(&c, 0, 0, 0xFF, 2, RAX, 0);
    movsd_store(&c, R12, 0, 0);
    store_steps(&c);
    alu_ri(&c, ALU_ADD, 1, RSP, 8);
    pop(&c, R15);
    pop(&c, R14);
    pop(&c, R13);
    pop(&c, R12);
    pop(&c, RBX);
    pop(&c, RBP);
    emit(&c, 0xC3);
    unsigned char *code = c.failed ? NULL : executable(c.sections[HOT].bytes, c.sections[HOT].length, &mapped);
    compiler_free(&c);
    if (code) memcpy(&entry, &code, sizeof entry);
    return entry;
}

#if defined(__clang__)
__attribute__((no_sanitize("function")))
#endif
int jit_invoke(CtInterpreter *interpreter, const Token *name, Node *function,
               const CtValue *values, size_t count, CtValue *result) {
    (void)name;
    if (!jit_allowed(interpreter) || !compile(interpreter, function)) return 0;
    Trampoline enter = trampoline();
    if (!enter) return 0;
    size_t parameters = 0;
    for (Node *p = function->left; p; p = p->next) ++parameters;
    if (parameters != count) return 0;
    uint64_t integers[6] = {0};
    double reals[8] = {0};
    int integer_count = 0, real_count = 0;
    size_t index = 0;
    for (Node *p = function->left; p; p = p->next) {
        CtValue value = runtime_convert(interpreter, p->token, values[index++], p->type);
        if (interpreter->failed) {
            *result = (CtValue){.type = CT_INT};
            return 1;
        }
        if (is_real(p->type)) reals[real_count++] = value.as.real;
        else integers[integer_count++] = value.as.unsigned_integer;
    }
    struct JitCatch catch;
    catch.previous = interpreter->jit_catch;
    size_t objects = interpreter->jit_object_count;
    unsigned depth = interpreter->depth;
    uintptr_t base = STACK_HERE(), budget = interpreter->depth_limit > depth ? interpreter->depth_limit - depth : 0;
    budget *= JIT_STACK_PER_DEPTH;
    catch.base = base;
    catch.depth = depth;
    catch.floor = interpreter->jit_floor;
    interpreter->jit_floor = budget < base ? base - budget : 0;
    interpreter->jit_catch = &catch;
    CtType returns = type_target(function->type);
    if (setjmp(catch.unwind)) {
        interpreter->jit_catch = catch.previous;
        interpreter->jit_floor = catch.floor;
        release_objects(interpreter, objects);
        interpreter->depth = depth;
        *result = interpreter->exit_requested && !interpreter->failed
            ? (CtValue){.type = CT_INT, .as.integer = interpreter->exit_status} : (CtValue){.type = CT_INT};
        return 1;
    }
    uint64_t bits = enter(interpreter, function->jit->body, integers, reals);
    interpreter->jit_catch = catch.previous;
    interpreter->jit_floor = catch.floor;
    if (returns == CT_VOID) *result = (CtValue){.type = CT_VOID};
    else if (is_real(returns)) *result = (CtValue){.type = CT_DOUBLE, .as.real = reals[0]};
    else *result = value_of(returns, bits);
    return 1;
}

#endif
