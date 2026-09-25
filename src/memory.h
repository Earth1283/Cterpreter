#ifndef CT_MEMORY_H
#define CT_MEMORY_H

#include "cterpreter.h"
#include "types.h"

#include <string.h>

/* Scalars and small aggregates keep their bytes and bitmap inside the record,
 * so declaring a local costs no host allocation. */
#define ALLOCATION_INLINE_BYTES 16

typedef struct Allocation Allocation;
struct Allocation {
    uint64_t address;
    size_t size;
    unsigned char *data, *initialized;
    int alive, heap, readonly;
    int fully_initialized; /* when set, the per-byte flags are stale and unused */
    size_t uninitialized; /* bytes whose initialized flag is still clear */
    Allocation *next_spare;
    unsigned char storage[2 * ALLOCATION_INLINE_BYTES];
};

#define MEMORY_FIND_CACHE_SIZE 256

typedef struct {
    Allocation **entries;
    uint64_t *addresses; /* entries[i]->address, contiguous for the binary search */
    size_t count, capacity, dead;
    Allocation *recent;
    Allocation *spares;
    Allocation *find_cache[MEMORY_FIND_CACHE_SIZE];
    uint64_t next_address;
    size_t bytes;
    const char *error;
} Memory;

uint64_t memory_allocate(Memory *memory, size_t size, int zero, int heap);
Allocation *memory_allocate_record(Memory *memory, size_t size, int zero, int heap);
/* Ends the lifetime of a live record, as memory_release does for its address. */
void memory_release_record(Memory *memory, Allocation *allocation);
Allocation *memory_find_slow(Memory *memory, uint64_t address, size_t cache_slot);
static inline Allocation *memory_find(Memory *memory, uint64_t address) {
    Allocation *recent = memory->recent;
    if (recent && address - recent->address <= recent->size) return recent;
    size_t cache_slot = (size_t)((address >> 4) & (MEMORY_FIND_CACHE_SIZE - 1));
    Allocation *cached = memory->find_cache[cache_slot];
    if (cached && address - cached->address <= cached->size) return memory->recent = cached;
    return memory_find_slow(memory, address, cache_slot);
}
void *memory_access(Memory *memory, uint64_t address, size_t size, int write);
/* Sets the initialized flags of a byte range, noticing when the whole object is. */
static inline void memory_mark(Allocation *allocation, size_t offset, size_t size) {
    if (allocation->fully_initialized) return;
    unsigned char *flags = allocation->initialized + offset;
    size_t fresh = 0;
    for (size_t i = 0; i < size; ++i) {
        fresh += !flags[i];
        flags[i] = 1;
    }
    allocation->uninitialized -= fresh;
    if (!allocation->uninitialized) allocation->fully_initialized = 1;
}
void memory_recount(Allocation *allocation);
int memory_release(Memory *memory, uint64_t address, int heap_only);
/* Raw scalar conversions for callers that have already validated the access. */
#define READ_AS(host, field) do { host stored; memcpy(&stored, data, sizeof stored); value.as.field = stored; } while (0)
#define WRITE_AS(host, field) do { host stored = (host)value.as.field; memcpy(data, &stored, sizeof stored); } while (0)

static inline CtValue memory_decode(const unsigned char *data, CtType type) {
    CtValue value = {.type = type};
    switch (type_kind(type)) {
        case TY_BOOL: READ_AS(_Bool, integer); break;
        case TY_CHAR: READ_AS(char, integer); break;
        case TY_SCHAR: READ_AS(signed char, integer); break;
        case TY_UCHAR: READ_AS(unsigned char, unsigned_integer); break;
        case TY_SHORT: READ_AS(short, integer); break;
        case TY_USHORT: READ_AS(unsigned short, unsigned_integer); break;
        case TY_INT: READ_AS(int, integer); break;
        case TY_UINT: READ_AS(unsigned, unsigned_integer); break;
        case TY_LONG: READ_AS(long, integer); break;
        case TY_ULONG: READ_AS(unsigned long, unsigned_integer); break;
        case TY_LLONG: READ_AS(long long, integer); break;
        case TY_ULLONG: READ_AS(unsigned long long, unsigned_integer); break;
        case TY_FLOAT: READ_AS(float, real); break;
        case TY_DOUBLE: READ_AS(double, real); break;
        default: READ_AS(uint64_t, address); break;
    }
    return value;
}

static inline void memory_encode(unsigned char *data, CtValue value) {
    switch (type_kind(value.type)) {
        case TY_BOOL: WRITE_AS(_Bool, integer); break;
        case TY_CHAR: WRITE_AS(char, integer); break;
        case TY_SCHAR: WRITE_AS(signed char, integer); break;
        case TY_UCHAR: WRITE_AS(unsigned char, unsigned_integer); break;
        case TY_SHORT: WRITE_AS(short, integer); break;
        case TY_USHORT: WRITE_AS(unsigned short, unsigned_integer); break;
        case TY_INT: WRITE_AS(int, integer); break;
        case TY_UINT: WRITE_AS(unsigned, unsigned_integer); break;
        case TY_LONG: WRITE_AS(long, integer); break;
        case TY_ULONG: WRITE_AS(unsigned long, unsigned_integer); break;
        case TY_LLONG: WRITE_AS(long long, integer); break;
        case TY_ULLONG: WRITE_AS(unsigned long long, unsigned_integer); break;
        case TY_FLOAT: WRITE_AS(float, real); break;
        case TY_DOUBLE: WRITE_AS(double, real); break;
        default: WRITE_AS(uint64_t, address); break;
    }
}

CtValue memory_read(Memory *memory, uint64_t address, CtType type);
int memory_write(Memory *memory, uint64_t address, CtValue value);
char *memory_string(Memory *memory, uint64_t address);
void memory_destroy(Memory *memory);

#endif
