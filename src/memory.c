#include "memory.h"
#include "types.h"

#include <stdlib.h>
#include <string.h>

/* Freed objects stay in the table so that a later access can say the lifetime
 * ended rather than that the pointer was never valid. Only the most recent
 * deaths are worth that much memory in a long-running loop; older ones are
 * forgotten and their addresses report an invalid access instead. */
#define REMEMBERED_DEAD 4096

static void forget_oldest_dead(Memory *memory) {
    size_t split = memory->count, remembered = 0;
    while (split && remembered < REMEMBERED_DEAD)
        if (!memory->entries[--split]->alive) ++remembered;
    size_t kept = 0;
    /* Some of the records below may be recycled. Invalidating the tiny lookup
     * caches is cheaper and less error-prone than finding each cached record. */
    memset(memory->find_cache, 0, sizeof memory->find_cache);
    memory->recent = NULL;
    for (size_t i = 0; i < split; ++i) {
        Allocation *allocation = memory->entries[i];
        if (allocation->alive) {
            memory->entries[kept] = allocation;
            memory->addresses[kept++] = allocation->address;
        } else {
            allocation->next_spare = memory->spares;
            memory->spares = allocation;
            --memory->dead;
        }
    }
    size_t moved = memory->count - split;
    memmove(memory->entries + kept, memory->entries + split, moved * sizeof *memory->entries);
    memmove(memory->addresses + kept, memory->addresses + split, moved * sizeof *memory->addresses);
    memory->count = kept + moved;
}

static int remember(Memory *memory, Allocation *allocation) {
    if (memory->dead > 2 * REMEMBERED_DEAD) forget_oldest_dead(memory);
    if (memory->count == memory->capacity) {
        size_t capacity = memory->capacity ? memory->capacity * 2 : 64;
        Allocation **entries = realloc(memory->entries, capacity * sizeof *entries);
        if (!entries) return 0;
        memory->entries = entries;
        uint64_t *addresses = realloc(memory->addresses, capacity * sizeof *addresses);
        if (!addresses) return 0;
        memory->addresses = addresses;
        memory->capacity = capacity;
    }
    memory->entries[memory->count] = allocation;
    memory->addresses[memory->count++] = allocation->address;
    return 1;
}

static Allocation *new_record(Memory *memory) {
    Allocation *allocation = memory->spares;
    if (allocation) memory->spares = allocation->next_spare;
    else if (!(allocation = malloc(sizeof *allocation))) return NULL;
    memset(allocation, 0, sizeof *allocation);
    return allocation;
}

static void recycle_record(Memory *memory, Allocation *allocation) {
    if (allocation->data != allocation->storage) free(allocation->data);
    allocation->next_spare = memory->spares;
    memory->spares = allocation;
}

uint64_t memory_allocate(Memory *memory, size_t size, int zero, int heap) {
    memory->error = NULL;
    if (size > 64u * 1024u * 1024u || memory->bytes > 64u * 1024u * 1024u - size) {
        memory->error = "interpreter memory limit exceeded";
        return 0;
    }
    Allocation *allocation = new_record(memory);
    if (!allocation) { memory->error = "out of memory"; return 0; }
    if (size <= ALLOCATION_INLINE_BYTES) {
        allocation->data = allocation->storage;
        allocation->initialized = allocation->storage + ALLOCATION_INLINE_BYTES;
    } else {
        /* Object bytes and their initialization bitmap share one allocation.
         * They have identical lifetimes and are accessed together. */
        allocation->data = calloc(size * 2, 1);
        allocation->initialized = allocation->data ? allocation->data + size : NULL;
    }
    if (!memory->next_address) memory->next_address = 4096;
    allocation->address = memory->next_address;
    if (!allocation->data || !remember(memory, allocation)) {
        recycle_record(memory, allocation);
        memory->error = "out of memory";
        return 0;
    }
    if (zero) memset(allocation->initialized, 1, size);
    memory->next_address += ((uint64_t)size + 31u) & ~UINT64_C(15);
    allocation->size = size;
    allocation->alive = 1;
    allocation->heap = heap;
    allocation->fully_initialized = zero || !size;
    memory->bytes += size;
    memory->find_cache[(allocation->address >> 4) & (MEMORY_FIND_CACHE_SIZE - 1)] = allocation;
    memory->recent = allocation;
    return allocation->address;
}

/* Addresses are handed out in increasing order and separated by at least
 * sixteen bytes, so the table stays sorted and one object's one-past-the-end
 * address can never belong to the next object. */
Allocation *memory_find_slow(Memory *memory, uint64_t address, size_t cache_slot) {
    size_t low = 0, high = memory->count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (memory->addresses[middle] <= address) low = middle + 1;
        else high = middle;
    }
    if (!low) return NULL;
    Allocation *allocation = memory->entries[low - 1];
    if (address - allocation->address > allocation->size) return NULL;
    memory->find_cache[cache_slot] = allocation;
    memory->recent = allocation;
    return allocation;
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
    if (!write && !allocation->fully_initialized) {
        if (memchr(allocation->initialized + offset, 0, size)) {
            memory->error = "read of uninitialized memory";
            return NULL;
        }
    } else if (write == 1) {
        memset(allocation->initialized + offset, 1, size);
        if (!offset && size == allocation->size) allocation->fully_initialized = 1;
    }
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
    if (allocation->data != allocation->storage) free(allocation->data);
    allocation->data = NULL;
    allocation->initialized = NULL;
    allocation->alive = 0;
    ++memory->dead;
    memory->bytes -= allocation->size;
    return 1;
}

#define READ_AS(host, field) do { host stored; memcpy(&stored, data, sizeof stored); value.as.field = stored; } while (0)
#define WRITE_AS(host, field) do { host stored = (host)value.as.field; memcpy(data, &stored, sizeof stored); } while (0)

CtValue memory_decode(const unsigned char *data, CtType type) {
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

void memory_encode(unsigned char *data, CtValue value) {
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

CtValue memory_read(Memory *memory, uint64_t address, CtType type) {
    const TypeInfo *info = type_info(type);
    if (!(info->kind <= TY_DOUBLE || info->kind == TY_POINTER)) {
        memory->error = "cannot read a void or aggregate object directly";
        return (CtValue){.type = type};
    }
    if (address & (info->align - 1)) { memory->error = "misaligned memory access"; return (CtValue){.type = type}; }
    const unsigned char *data = memory_access(memory, address, info->size, 0);
    return data ? memory_decode(data, type) : (CtValue){.type = type};
}

int memory_write(Memory *memory, uint64_t address, CtValue value) {
    const TypeInfo *info = type_info(value.type);
    if (!(info->kind <= TY_DOUBLE || info->kind == TY_POINTER)) {
        memory->error = "cannot write a void or aggregate object directly";
        return 0;
    }
    if (address & (info->align - 1)) { memory->error = "misaligned memory access"; return 0; }
    unsigned char *data = memory_access(memory, address, info->size, 1);
    if (!data) return 0;
    memory_encode(data, value);
    return 1;
}

char *memory_string(Memory *memory, uint64_t address) {
    Allocation *allocation = memory_find(memory, address);
    char *start = memory_access(memory, address, 0, 0);
    if (!start) return NULL;
    size_t available = allocation->size - (size_t)(address - allocation->address);
    if (allocation->fully_initialized) {
        if (memchr(start, 0, available)) return start;
        memory->error = "string is not NUL terminated within its object";
        return NULL;
    }
    for (size_t i = 0; i < available; ++i) {
        size_t offset = (size_t)(address - allocation->address) + i;
        if (!allocation->initialized[offset]) { memory->error = "uninitialized byte in string"; return NULL; }
        if (!start[i]) return start;
    }
    memory->error = "string is not NUL terminated within its object";
    return NULL;
}

void memory_destroy(Memory *memory) {
    for (size_t i = 0; i < memory->count; ++i) {
        Allocation *allocation = memory->entries[i];
        if (allocation->data != allocation->storage) free(allocation->data);
        free(allocation);
    }
    while (memory->spares) {
        Allocation *next = memory->spares->next_spare;
        free(memory->spares);
        memory->spares = next;
    }
    free(memory->entries);
    free(memory->addresses);
    *memory = (Memory){0};
}
