#include "memory.h"
#include "types.h"

#include <stdlib.h>
#include <string.h>

uint64_t memory_allocate(Memory *memory, size_t size, int zero, int heap) {
    memory->error = NULL;
    if (size > 64u * 1024u * 1024u || memory->bytes > 64u * 1024u * 1024u - size) {
        memory->error = "interpreter memory limit exceeded";
        return 0;
    }
    Allocation *allocation = calloc(1, sizeof *allocation);
    if (allocation) {
        allocation->data = calloc(size ? size : 1, 1);
        allocation->initialized = malloc(size ? size : 1);
    }
    if (!allocation || !allocation->data || !allocation->initialized) {
        if (allocation) { free(allocation->data); free(allocation->initialized); }
        free(allocation);
        memory->error = "out of memory";
        return 0;
    }
    memset(allocation->initialized, zero ? 1 : 0, size);
    if (!memory->next_address) memory->next_address = 4096;
    allocation->address = memory->next_address;
    memory->next_address += ((uint64_t)size + 31u) & ~UINT64_C(15);
    allocation->size = size;
    allocation->alive = 1;
    allocation->heap = heap;
    allocation->next = memory->allocations;
    memory->allocations = allocation;
    memory->bytes += size;
    return allocation->address;
}

Allocation *memory_find(Memory *memory, uint64_t address) {
    for (Allocation *allocation = memory->allocations; allocation; allocation = allocation->next)
        if (address >= allocation->address && address - allocation->address <= allocation->size)
            return allocation;
    return NULL;
}

void *memory_access(Memory *memory, uint64_t address, size_t size, int write) {
    memory->error = NULL;
    Allocation *allocation = memory_find(memory, address);
    if (!address) memory->error = "null pointer access";
    else if (!allocation) memory->error = "invalid pointer access";
    else if (!allocation->alive) memory->error = "access to an object whose lifetime has ended";
    else if (size > allocation->size - (size_t)(address - allocation->address))
        memory->error = "access outside object bounds";
    else if (write && allocation->readonly) memory->error = "write to a read-only object";
    if (memory->error) return NULL;
    size_t offset = (size_t)(address - allocation->address);
    if (!write) {
        for (size_t i = 0; i < size; ++i)
            if (!allocation->initialized[offset + i]) {
                memory->error = "read of uninitialized memory";
                return NULL;
            }
    } else if (write == 1) memset(allocation->initialized + offset, 1, size);
    return allocation->data + offset;
}

int memory_release(Memory *memory, uint64_t address, int heap_only) {
    memory->error = NULL;
    if (!address) return 1;
    Allocation *allocation = memory_find(memory, address);
    if (!allocation || allocation->address != address) memory->error = "free requires an allocation's starting address";
    else if (!allocation->alive) memory->error = "object was already freed";
    else if (heap_only && !allocation->heap) memory->error = "free requires a heap allocation";
    if (memory->error) return 0;
    free(allocation->data);
    free(allocation->initialized);
    allocation->data = NULL;
    allocation->initialized = NULL;
    allocation->alive = 0;
    memory->bytes -= allocation->size;
    return 1;
}

CtValue memory_read(Memory *memory, uint64_t address, CtType type) {
    CtValue value = {.type = type};
    size_t size = ct_type_size(type);
    if (!type_is_scalar(type)) { memory->error = "cannot read a void or aggregate object directly"; return value; }
    if (address % ct_type_align(type)) { memory->error = "misaligned memory access"; return value; }
    void *data = memory_access(memory, address, size, 0);
    if (!data) return value;
    if (type_is_pointer(type)) memcpy(&value.as.address, data, size);
    else if (type == CT_DOUBLE) memcpy(&value.as.real, data, size);
    else if (type == CT_CHAR) value.as.integer = *(char *)data;
    else memcpy(&value.as.integer, data, size);
    return value;
}

int memory_write(Memory *memory, uint64_t address, CtValue value) {
    size_t size = ct_type_size(value.type);
    if (!type_is_scalar(value.type)) { memory->error = "cannot write a void or aggregate object directly"; return 0; }
    if (address % ct_type_align(value.type)) { memory->error = "misaligned memory access"; return 0; }
    void *data = memory_access(memory, address, size, 1);
    if (!data) return 0;
    if (type_is_pointer(value.type)) memcpy(data, &value.as.address, size);
    else if (value.type == CT_DOUBLE) memcpy(data, &value.as.real, size);
    else if (value.type == CT_CHAR) *(char *)data = (char)value.as.integer;
    else memcpy(data, &value.as.integer, size);
    return 1;
}

char *memory_string(Memory *memory, uint64_t address) {
    Allocation *allocation = memory_find(memory, address);
    char *start = memory_access(memory, address, 0, 0);
    if (!start) return NULL;
    size_t available = allocation->size - (size_t)(address - allocation->address);
    for (size_t i = 0; i < available; ++i) {
        size_t offset = (size_t)(address - allocation->address) + i;
        if (!allocation->initialized[offset]) { memory->error = "uninitialized byte in string"; return NULL; }
        if (!start[i]) return start;
    }
    memory->error = "string is not NUL terminated within its object";
    return NULL;
}

void memory_destroy(Memory *memory) {
    Allocation *allocation = memory->allocations;
    while (allocation) {
        Allocation *next = allocation->next;
        free(allocation->data);
        free(allocation->initialized);
        free(allocation);
        allocation = next;
    }
    *memory = (Memory){0};
}
