/*
 * Sorting functions for VexRiscv
 * Uses iterative quicksort to avoid stack overflow
 */

#include "libc.h"

/* Swap two elements */
static void swap(void *a, void *b, size_t size) {
    uint8_t *pa = (uint8_t *)a;
    uint8_t *pb = (uint8_t *)b;

    while (size > 0) {
        uint8_t tmp = *pa;
        *pa++ = *pb;
        *pb++ = tmp;
        size--;
    }
}

/* Get element at index */
static void *elem_at(void *base, size_t index, size_t size) {
    return (uint8_t *)base + index * size;
}

/* Partition for quicksort */
static size_t partition(void *base, size_t left, size_t right, size_t size,
                        int (*compar)(const void *, const void *)) {
    /* Use middle element as pivot (better for sorted arrays) */
    size_t mid = left + (right - left) / 2;
    swap(elem_at(base, mid, size), elem_at(base, right, size), size);

    void *pivot = elem_at(base, right, size);
    size_t i = left;

    for (size_t j = left; j < right; j++) {
        if (compar(elem_at(base, j, size), pivot) < 0) {
            swap(elem_at(base, i, size), elem_at(base, j, size), size);
            i++;
        }
    }

    swap(elem_at(base, i, size), elem_at(base, right, size), size);
    return i;
}

/* Stack for iterative quicksort */
#define QSORT_STACK_SIZE 64  /* Enough for 2^64 elements */

void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *)) {
    if (nmemb <= 1 || size == 0 || base == NULL) {
        return;
    }

    /* Use stack-based iterative approach */
    size_t stack[QSORT_STACK_SIZE];
    int top = -1;

    /* Push initial range */
    stack[++top] = 0;
    stack[++top] = nmemb - 1;

    while (top >= 0) {
        /* Pop range */
        size_t right = stack[top--];
        size_t left = stack[top--];

        if (left >= right) {
            continue;
        }

        /* For small subarrays, use insertion sort */
        if (right - left < 10) {
            for (size_t i = left + 1; i <= right; i++) {
                size_t j = i;
                while (j > left && compar(elem_at(base, j - 1, size),
                                           elem_at(base, j, size)) > 0) {
                    swap(elem_at(base, j - 1, size), elem_at(base, j, size), size);
                    j--;
                }
            }
            continue;
        }

        /* Partition */
        size_t p = partition(base, left, right, size, compar);

        /* Push larger subarray first (ensures smaller stack usage) */
        if (p > 0 && p - 1 - left > right - p - 1) {
            /* Left is larger */
            if (p > 0) {
                stack[++top] = left;
                stack[++top] = p - 1;
            }
            if (p + 1 < right) {
                stack[++top] = p + 1;
                stack[++top] = right;
            }
        } else {
            /* Right is larger */
            if (p + 1 < right) {
                stack[++top] = p + 1;
                stack[++top] = right;
            }
            if (p > 0) {
                stack[++top] = left;
                stack[++top] = p - 1;
            }
        }
    }
}

void *bsearch(const void *key, const void *base, size_t nmemb, size_t size,
              int (*compar)(const void *, const void *)) {
    size_t left = 0;
    size_t right = nmemb;

    while (left < right) {
        size_t mid = left + (right - left) / 2;
        const void *elem = (const uint8_t *)base + mid * size;
        int cmp = compar(key, elem);

        if (cmp == 0) {
            return (void *)elem;
        } else if (cmp < 0) {
            right = mid;
        } else {
            left = mid + 1;
        }
    }

    return NULL;
}
