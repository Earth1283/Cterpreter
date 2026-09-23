#ifndef CT_MEMORY_H
#define CT_MEMORY_H

#include "cterpreter.h"

/* Scalars and small aggregates keep their bytes and bitmap inside the record,
 * so declaring a local costs no host allocation. */
#define ALLOCATION_INLINE_BYTES 16

typedef struct Allocation Allocation;
struct Allocation {
    uint64_t address;
    size_t size;
    unsigned char *data, *initialized;
    int alive, heap, readonly, fully_initialized;
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
int memory_release(Memory *memory, uint64_t address, int heap_only);
/* Raw scalar conversions for callers that have already validated the access. */
CtValue memory_decode(const unsigned char *data, CtType type);
void memory_encode(unsigned char *data, CtValue value);
CtValue memory_read(Memory *memory, uint64_t address, CtType type);
int memory_write(Memory *memory, uint64_t address, CtValue value);
char *memory_string(Memory *memory, uint64_t address);
void memory_destroy(Memory *memory);

#endif
