/* A page for hook stubs inside a free gap of the address space (probe.c allocate_near_image,
 * POSIX): the gap [gap_lo, gap_hi) is first clipped to the window [lo, hi) that rel32 jumps from
 * the image reach, then the first page-aligned start in what is left is taken. 0 when no whole
 * page fits. Clipping first matters: a gap that starts below the window (the free space under
 * the image) still holds pages inside it, and its own start would be out of jump range. */
#pragma once
#include <stdint.h>

static inline uint64_t near_gap_page(uint64_t gap_lo, uint64_t gap_hi, uint64_t lo, uint64_t hi,
                                     uint64_t page) {
    if (gap_lo < lo) gap_lo = lo;
    if (gap_hi > hi) gap_hi = hi;
    if (gap_lo >= gap_hi) return 0;
    const uint64_t start = (gap_lo + page - 1) & ~(page - 1);
    return start >= gap_lo && start <= gap_hi && gap_hi - start >= page ? start : 0;
}
