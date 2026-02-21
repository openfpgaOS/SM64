/*
 * Memory allocation and operations for VexRiscv
 * Simple first-fit allocator with coalescing
 */

#include "libc.h"

/* ============================================
 * Heap allocator
 * ============================================ */

/* Block header - 8 bytes aligned */
typedef struct block_header {
    uint32_t size;          /* Size including header, bit 0 = used flag */
    uint32_t prev_size;     /* Previous block size (for coalescing) */
} block_header_t;

#define BLOCK_USED      0x1
#define BLOCK_SIZE_MASK (~0x3)
#define MIN_BLOCK_SIZE  16      /* Minimum allocation: header + 8 bytes */
#define ALIGNMENT       8

static uint8_t *heap_start = NULL;
static uint8_t *heap_end = NULL;
static block_header_t *free_list = NULL;

void heap_init(void *start, size_t size) {
    /* Align start to 8-byte boundary */
    uintptr_t aligned_start = ((uintptr_t)start + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
    size -= (aligned_start - (uintptr_t)start);
    size &= ~(ALIGNMENT - 1);  /* Align size down */

    heap_start = (uint8_t *)aligned_start;
    heap_end = heap_start + size;

    /* Create initial free block spanning entire heap */
    block_header_t *initial = (block_header_t *)heap_start;
    initial->size = size;  /* Not used (bit 0 = 0) */
    initial->prev_size = 0;

    free_list = initial;
}

static size_t align_size(size_t size) {
    size_t total = size + sizeof(block_header_t);
    if (total < MIN_BLOCK_SIZE) {
        total = MIN_BLOCK_SIZE;
    }
    return (total + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
}

void *malloc(size_t size) {
    if (size == 0 || heap_start == NULL) {
        return NULL;
    }

    size_t needed = align_size(size);

    /* First-fit search through heap */
    block_header_t *block = (block_header_t *)heap_start;

    while ((uint8_t *)block < heap_end) {
        uint32_t block_size = block->size & BLOCK_SIZE_MASK;

        if (!(block->size & BLOCK_USED) && block_size >= needed) {
            /* Found a free block large enough */

            /* Split if there's enough space for another block */
            if (block_size >= needed + MIN_BLOCK_SIZE) {
                block_header_t *next = (block_header_t *)((uint8_t *)block + needed);
                next->size = block_size - needed;
                next->prev_size = needed;

                /* Update the block after next, if it exists */
                block_header_t *after_next = (block_header_t *)((uint8_t *)next + (next->size & BLOCK_SIZE_MASK));
                if ((uint8_t *)after_next < heap_end) {
                    after_next->prev_size = next->size & BLOCK_SIZE_MASK;
                }

                block->size = needed | BLOCK_USED;
            } else {
                /* Use entire block */
                block->size |= BLOCK_USED;
            }

            /* Return pointer after header */
            return (void *)((uint8_t *)block + sizeof(block_header_t));
        }

        /* Move to next block */
        block = (block_header_t *)((uint8_t *)block + block_size);
    }

    /* No suitable block found */
    return NULL;
}

void *calloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;

    /* Check for overflow */
    if (nmemb != 0 && total / nmemb != size) {
        return NULL;
    }

    void *ptr = malloc(total);
    if (ptr != NULL) {
        memset(ptr, 0, total);
    }
    return ptr;
}

void free(void *ptr) {
    if (ptr == NULL || heap_start == NULL) {
        return;
    }

    /* Get block header */
    block_header_t *block = (block_header_t *)((uint8_t *)ptr - sizeof(block_header_t));

    /* Validate pointer is within heap */
    if ((uint8_t *)block < heap_start || (uint8_t *)block >= heap_end) {
        return;  /* Invalid pointer */
    }

    /* Mark as free */
    block->size &= ~BLOCK_USED;

    uint32_t block_size = block->size & BLOCK_SIZE_MASK;

    /* Coalesce with next block if free */
    block_header_t *next = (block_header_t *)((uint8_t *)block + block_size);
    if ((uint8_t *)next < heap_end && !(next->size & BLOCK_USED)) {
        uint32_t next_size = next->size & BLOCK_SIZE_MASK;
        block->size += next_size;
        block_size += next_size;

        /* Update block after merged */
        block_header_t *after_merged = (block_header_t *)((uint8_t *)block + block_size);
        if ((uint8_t *)after_merged < heap_end) {
            after_merged->prev_size = block_size;
        }
    }

    /* Coalesce with previous block if free */
    if (block->prev_size != 0) {
        block_header_t *prev = (block_header_t *)((uint8_t *)block - block->prev_size);
        if ((uint8_t *)prev >= heap_start && !(prev->size & BLOCK_USED)) {
            uint32_t prev_size = prev->size & BLOCK_SIZE_MASK;
            prev->size = (prev_size + block_size);

            /* Update block after merged */
            block_header_t *after_merged = (block_header_t *)((uint8_t *)prev + (prev->size & BLOCK_SIZE_MASK));
            if ((uint8_t *)after_merged < heap_end) {
                after_merged->prev_size = prev->size & BLOCK_SIZE_MASK;
            }
        }
    }
}

void *realloc(void *ptr, size_t size) {
    if (ptr == NULL) {
        return malloc(size);
    }

    if (size == 0) {
        free(ptr);
        return NULL;
    }

    /* Get current block size */
    block_header_t *block = (block_header_t *)((uint8_t *)ptr - sizeof(block_header_t));
    uint32_t current_size = (block->size & BLOCK_SIZE_MASK) - sizeof(block_header_t);

    /* If new size fits in current block, just return */
    size_t needed = align_size(size);
    if (needed <= (block->size & BLOCK_SIZE_MASK)) {
        return ptr;
    }

    /* Allocate new block and copy */
    void *new_ptr = malloc(size);
    if (new_ptr != NULL) {
        size_t copy_size = (size < current_size) ? size : current_size;
        memcpy(new_ptr, ptr, copy_size);
        free(ptr);
    }

    return new_ptr;
}

/* ============================================
 * Memory operations
 * ============================================ */

void *memcpy(void *dest, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;

    /* Word-aligned copy for better performance */
    if (((uintptr_t)d & 3) == 0 && ((uintptr_t)s & 3) == 0) {
        uint32_t *d32 = (uint32_t *)d;
        const uint32_t *s32 = (const uint32_t *)s;

        while (n >= 4) {
            *d32++ = *s32++;
            n -= 4;
        }

        d = (uint8_t *)d32;
        s = (const uint8_t *)s32;
    }

    /* Copy remaining bytes */
    while (n > 0) {
        *d++ = *s++;
        n--;
    }

    return dest;
}

void *memset(void *s, int c, size_t n) {
    uint8_t *p = (uint8_t *)s;
    uint8_t val = (uint8_t)c;

    /* Word-aligned fill for better performance */
    if (((uintptr_t)p & 3) == 0) {
        uint32_t val32 = val | (val << 8) | (val << 16) | (val << 24);
        uint32_t *p32 = (uint32_t *)p;

        while (n >= 4) {
            *p32++ = val32;
            n -= 4;
        }

        p = (uint8_t *)p32;
    }

    /* Fill remaining bytes */
    while (n > 0) {
        *p++ = val;
        n--;
    }

    return s;
}

void *memmove(void *dest, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;

    if (d == s || n == 0) {
        return dest;
    }

    /* If dest is before src, copy forward */
    if (d < s) {
        return memcpy(dest, src, n);
    }

    /* If dest is after src, copy backward */
    d += n;
    s += n;

    while (n > 0) {
        *--d = *--s;
        n--;
    }

    return dest;
}

/* BSD-style memory functions used by SM64 */
void bcopy(const void *src, void *dest, size_t n) {
    memmove(dest, src, n);
}

void bzero(void *s, size_t n) {
    memset(s, 0, n);
}

int memcmp(const void *s1, const void *s2, size_t n) {
    const uint8_t *p1 = (const uint8_t *)s1;
    const uint8_t *p2 = (const uint8_t *)s2;

    while (n > 0) {
        if (*p1 != *p2) {
            return (*p1 < *p2) ? -1 : 1;
        }
        p1++;
        p2++;
        n--;
    }

    return 0;
}
