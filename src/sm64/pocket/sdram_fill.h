//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * SDRAM Fill Engine — hardware DMA for memory fills
 *
 * Fills a contiguous SDRAM region with a constant 32-bit pattern.
 * Operates via AXI4 DMA, does NOT go through the CPU cache.
 *
 * Use for uncached SDRAM regions (framebuffer draw buffer at 0x50xxxxxx).
 * Do NOT use for cached SDRAM without invalidating D-cache afterward.
 */
#ifndef SDRAM_FILL_H
#define SDRAM_FILL_H

#include <stdint.h>

#define FILL_BASE       0x44000000
#define FILL_ADDR       (*(volatile uint32_t *)(FILL_BASE + 0x00))
#define FILL_LENGTH     (*(volatile uint32_t *)(FILL_BASE + 0x04))
#define FILL_DATA       (*(volatile uint32_t *)(FILL_BASE + 0x08))
#define FILL_CTRL       (*(volatile uint32_t *)(FILL_BASE + 0x0C))
#define FILL_STATUS     (*(volatile uint32_t *)(FILL_BASE + 0x10))

/* Start a fill operation. addr must be word-aligned, len in bytes (multiple of 4). */
static inline void sdram_fill_start(uint32_t addr, uint32_t len, uint32_t pattern) {
    FILL_ADDR   = addr;
    FILL_LENGTH = len;
    FILL_DATA   = pattern;
    FILL_CTRL   = 1;
}

/* Poll until fill is complete. */
static inline void sdram_fill_wait(void) {
    while (FILL_STATUS & 1)
        ;
}

/* Check if fill is still in progress. */
static inline int sdram_fill_busy(void) {
    return FILL_STATUS & 1;
}

#endif /* SDRAM_FILL_H */
