/* The stub-page choice inside a free gap (src/near_gap.h, probe.c allocate_near_image on POSIX). */
#undef NDEBUG
#include "../src/near_gap.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    const uint64_t page = 0x1000, lo = 0x20000000, hi = 0xa0000000;
    /* A gap inside the window: its first aligned page. */
    assert(near_gap_page(0x30000123, 0x30010000, lo, hi, page) == 0x30001000);
    /* A gap that starts below the window: the page is the window's bottom, not the gap's start
     * (the gap's own start is more than the jump range away from the image). */
    assert(near_gap_page(0x10000, 0x40000000, lo, hi, page) == lo);
    /* A gap that ends above the window: clipped to it. */
    assert(near_gap_page(hi - page, 0xffff0000, lo, hi, page) == hi - page);
    assert(near_gap_page(hi - 0x800, 0xffff0000, lo, hi, page) == 0);
    /* Entirely outside the window, or too small for a page. */
    assert(near_gap_page(0x10000, 0x1000000, lo, hi, page) == 0);
    assert(near_gap_page(0xb0000000, 0xc0000000, lo, hi, page) == 0);
    assert(near_gap_page(0x30000800, 0x30001800, lo, hi, page) == 0);
    assert(near_gap_page(0x30001000, 0x30002000, lo, hi, page) == 0x30001000);
    puts("near-gap tests passed");
    return 0;
}
