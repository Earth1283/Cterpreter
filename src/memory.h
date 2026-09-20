#ifndef CT_MEMORY_H
#define CT_MEMORY_H

#include "cterpreter.h"

typedef struct Allocation Allocation;
struct Allocation {
    uint64_t address;
    size_t size;
    unsigned char *data, *initialized;
    int alive, heap, readonly;
    Allocation *next;
};

typedef struct {
    Allocation *allocations;
    uint64_t next_address;
    size_t bytes;
    const char *error;
} Memory;

uint64_t memory_allocate(Memory *memory, size_t size, int zero, int heap);
Allocation *memory_find(Memory *memory, uint64_t address);
void *memory_access(Memory *memory, uint64_t address, size_t size, int write);
int memory_release(Memory *memory, uint64_t address, int heap_only);
CtValue memory_read(Memory *memory, uint64_t address, CtType type);
int memory_write(Memory *memory, uint64_t address, CtValue value);
char *memory_string(Memory *memory, uint64_t address);
void memory_destroy(Memory *memory);

#endif
