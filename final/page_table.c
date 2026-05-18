 /*  A basic page table plus the virtual -> physical address translation
 * algorithm performed by the MMU, so I make sure I fully get every little bit calculation made.
 *
 *   - The virtual address space is split into fixed-size PAGES and
 *     physical memory into FRAMES (page frames) of the same size.
 *   - The MMU reads a virtual address as (page, offset): the n most
 *     significant bits are the page number, the rest is the offset.
 *     n depends on the page size (assuming byte addressable memory).
 *   - The page table maps page number -> frame number.
 *   - Each entry (PTE) holds: the frame, a present bit, and the
 *     protection / dirty / referenced bits (slide 22).
 *   - If the page is not present, the MMU raises a PAGE FAULT that the
 *     OS traps.
 *   - Cute realization: the mechanism is VERY, VERY similar to an n-way caching mechanism of splitting the bits of the address and adressing inside the cache (pretty much the same)
 *
 * Numbers are kept small so it is easy to follow, just like slide 20:
 * 16 pages of 4 KB.
 *
 * Build:  cc -Wall -Wextra -o page_table page_table.c
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* 16-bit virtual address, 4 KB pages.
 *
 *   |  4 bits  |       12 bits        |
 *   |   page   |        offset        |
 *
 *   2^12 = 4096  -> page size = 4 KB (byte addressable as any sane human would do).
 *   2^4  = 16    -> number of virtual pages
 */
#define OFFSET_BITS     12
#define PAGE_SIZE       (1u << OFFSET_BITS)         // 4096 (2**12) bytes
#define OFFSET_MASK     (PAGE_SIZE - 1u)            // 0x0FFF, basically we'll use this to retrieve just the last 12 bits and forget about the first 4 bits of the page.

#define NUM_PAGES       16                          // virtual pages
#define NUM_FRAMES      8                           // physical frames (later on if I am not lazy enough I'll make a TLB).

/*
 * Page Table Entry: one entry of the page table (slide 22).
 * Bitfields keep each PTE small, we don't want a huge ass PT just to state the state of memory */
typedef struct {
    uint32_t frame      : 3;   // frame number (page frame) - basically log_2(NUM_FRAMES).  
    uint32_t present    : 1;   // 1 = page is in physical memory
    uint32_t writable   : 1;   // protection bit: 1 = read/write
    uint32_t dirty      : 1;   // 1 = modified since it was loaded
    uint32_t referenced : 1;   // 1 = accessed since it was loaded
} PTE;

// A process's page table is simply an array of PTEs indexed by page number.
typedef struct {
    PTE entries[NUM_PAGES];
} PageTable;

// Result of asking the MMU to translate an address.
typedef enum {
    TR_OK,               // translation succeeded 
    TR_PAGE_FAULT,       // valid page but not present -> page fault
    TR_PROT_VIOLATION    // write to a read-only page
} TranslationResult;

void page_table_init(PageTable *t)
{
    for (int p = 0; p < NUM_PAGES; p++) {
        t->entries[p].frame      = 0;
        t->entries[p].present    = 0;
        t->entries[p].writable   = 1;
        t->entries[p].dirty      = 0;
        t->entries[p].referenced = 0;
    }
}

/* Map page `page` to frame `frame` (what the OS does when it loads a
 * page from disk after a page fault). */
void page_table_map(PageTable *t, uint32_t page, uint32_t frame, int writable)
{
    if (page >= NUM_PAGES) {
        fprintf(stderr, "page_table_map: page %u out of range\n", page);
        return;
    }
    t->entries[page].frame      = frame;
    t->entries[page].present    = 1;
    t->entries[page].writable   = writable ? 1 : 0;
    t->entries[page].dirty      = 0;
    t->entries[page].referenced = 0;
}

/*
 * Translate a virtual address into a physical one.
 *
 *   is_write = 1 if the access is a write (used to check protection
 *              and set the dirty bit).
 *
 * Returns the result; on TR_OK the physical address is left in
 * *physical_out.
 */
TranslationResult mmu_translate(PageTable *t, uint32_t virtual,
                                int is_write, uint32_t *physical_out)
{
    // Split the virtual address into (page, offset).
    uint32_t page   = virtual >> OFFSET_BITS;
    uint32_t offset = virtual & OFFSET_MASK;

    if (page >= NUM_PAGES) {
        // Address outside the virtual space: the OS kills the process. Well, it should.
        return TR_PAGE_FAULT;
    }

    PTE *pte = &t->entries[page];

    /* Present bit: if the page is not in memory -> page fault.
     *  Here the MMU raises an interrupt that the OS traps. NOT the same as addressing beyond the virtual space (previous case). */
    if (!pte->present) {
        return TR_PAGE_FAULT;
    }

    // Protection bit: write to a read-only page.
    if (is_write && !pte->writable) {
        return TR_PROT_VIOLATION;
    }

    // Update the usage bits (used by the page replacement algorithm, for example a Least Recently Used).
    pte->referenced = 1;
    if (is_write) {
        pte->dirty = 1;
    }

    /* 5. Physical address = frame * PAGE_SIZE + offset.
     *    The offset is copied verbatim (slide 20). */
    *physical_out = (pte->frame << OFFSET_BITS) | offset;
    return TR_OK;
}

void page_table_print(const PageTable *t)
{
    printf("   page | present | frame | W D R\n");
    printf("  ------+---------+-------+------\n");
    for (int p = 0; p < NUM_PAGES; p++) {
        const PTE *e = &t->entries[p];
        if (e->present) {
            printf("   %4d |   yes   |  %3u  | %u %u %u\n",
                   p, e->frame, e->writable, e->dirty, e->referenced);
        } else {
            printf("   %4d |   no    |   -   | -    (on disk / swap)\n", p);
        }
    }
}

// Ask for a translation and print the result in a readable way (thanks claude).
void try_access(PageTable *t, uint32_t virtual, int is_write)
{
    uint32_t physical;
    const char *mode = is_write ? "write" : "read ";
    TranslationResult r = mmu_translate(t, virtual, is_write, &physical);

    printf("  virtual %5u (page %2u, off %4u) [%s] -> ",
           virtual, virtual >> OFFSET_BITS, virtual & OFFSET_MASK, mode);

    switch (r) {
    case TR_OK:
        printf("physical %u (frame %u)\n", physical, physical >> OFFSET_BITS);
        break;
    case TR_PAGE_FAULT:
        printf("PAGE FAULT  (the OS must load the page)\n");
        break;
    case TR_PROT_VIOLATION:
        printf("PROTECTION VIOLATION (segmentation violation)\n");
        break;
    }
}

//                      DEMO 
//
int main(void)
{
    PageTable table;
    page_table_init(&table);

    /*
     * Reproduce the example from slide 20 (16 pages of 4 KB).
     * The page -> frame mapping is the one in Tanenbaum's figure.
     * Pages not listed stay "not present" (in swap).
     */
    page_table_map(&table, 0, 2, 1);
    page_table_map(&table, 1, 1, 1);
    page_table_map(&table, 2, 6, 1);    // virtual 8196 -> physical 24580
    page_table_map(&table, 3, 0, 1);
    page_table_map(&table, 4, 4, 1);
    page_table_map(&table, 5, 3, 1);
    page_table_map(&table, 6, 5, 0);    // read-only: e.g. the TEXT segment
    page_table_map(&table, 8, 7, 1);
    // pages 7, 9..15 -> not present

    printf("=== Process page table ===\n");
    page_table_print(&table);

    printf("\n=== Translations ===\n");
    try_access(&table, 8196,  0);  // slide 20: -> 24580
    try_access(&table, 0,     0);  // page 0, offset 0
    try_access(&table, 4097,  1);  // page 1, write -> sets dirty
    try_access(&table, 24600, 1);  // page 6, write -> protection fault
    try_access(&table, 32768, 0);  // page 8: present
    try_access(&table, 28672, 0);  // page 7: not present -> page fault

    /*
     * Handling the page fault: the OS would load the page from disk and
     * update the table. Here we simulate it by mapping page 7 to a free
     * frame and retrying the instruction (slides 28-29).
     */
    printf("\n=== The OS services the page fault for page 7 ===\n");
    page_table_map(&table, 7, 6, 1);   // loaded into a free frame
    try_access(&table, 28672, 0);      // retry: now it translates ok

    return 0;
}
