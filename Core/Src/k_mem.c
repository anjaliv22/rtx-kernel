#include "k_mem.h"
#include "k_task.h"
#include <stdint.h>
#include <stddef.h>

// _ebss is the end of the bss section from the linker script- accessing it as an array lets us get the address directly without &
extern uint32_t _ebss[];

#define RAM_TOP 0x20018000u
#define MSP_RESERVE 0x400u
#define HEAP_END_ADDR (RAM_TOP - MSP_RESERVE)
#define ALIGN4_UP(x)  (((uint32_t)(x) + 3u) & ~3u)

typedef struct blkhdr {
    uint32_t size;       // payload size (not counting this header)
    uint8_t is_free;
    uint8_t owner;      // TID of whoever allocated this
    uint16_t _pad;
    struct blkhdr *next_free;  // only used when block is free
} BlockHeader;

#define HDR_SZ  ((uint32_t)sizeof(BlockHeader))
static uint8_t g_inited = 0;
static uint32_t g_heap_start = 0;
static uint32_t g_heap_end = 0;
static BlockHeader *g_free_head = NULL;

// returns the block physically right after b in memory, or NULL if we're at the end
static BlockHeader *phys_next(const BlockHeader *b)
{
    uint32_t addr = (uint32_t)b + HDR_SZ + b->size;
    if (addr >= g_heap_end) return NULL;
    return (BlockHeader *)addr;
}

// insert blk into free list, keeping it sorted by address (low to high)
static void fl_insert(BlockHeader *blk)
{
    blk->is_free = 1;
    if (g_free_head == NULL || (uint32_t)blk < (uint32_t)g_free_head) {     // goes at the front if list is empty or blk has lowest address
        blk->next_free = g_free_head;
        g_free_head = blk;
        return;
    }

    // goes at the front if list is empty or blk has lowest address- goes at the front if list is empty or blk has lowest address
    BlockHeader *cur = g_free_head;
    while (cur->next_free != NULL && (uint32_t)cur->next_free < (uint32_t)blk) {
        cur = cur->next_free;
    }
    blk->next_free = cur->next_free;
    cur->next_free = blk;
}

// remove blk from the free list
// assumes blk is actually in the list, undefined behaviour otherwise
static void fl_remove(BlockHeader *blk)
{
    if (g_free_head == blk) {
        g_free_head = blk->next_free;
        blk->next_free = NULL;
        return;
    }

    BlockHeader *cur = g_free_head;
    while (cur != NULL && cur->next_free != blk) {
        cur = cur->next_free;
    }

    if (cur != NULL) {
        cur->next_free = blk->next_free;
    }
    blk->next_free = NULL;
}

// SVC wrappers (user-facing

int k_mem_init(void)
{
    register int r0 __asm("r0") = 0;
    __asm volatile ("svc %1" : "+r"(r0) : "I"(SVC_MEM_INIT) : "memory");
    return r0;
}

void *k_mem_alloc(size_t size)
{
    register uint32_t r0 __asm("r0") = (uint32_t)size;
    __asm volatile ("svc %1" : "+r"(r0) : "I"(SVC_MEM_ALLOC) : "memory");
    return (void *)r0;
}

int k_mem_dealloc(void *ptr)
{
    register uint32_t r0 __asm("r0") = (uint32_t)ptr;
    __asm volatile ("svc %1" : "+r"(r0) : "I"(SVC_MEM_DEALLOC) : "memory");
    return (int)r0;
}

int k_mem_count_extfrag(size_t size)
{
    register int r0 __asm("r0") = (int)size;
    __asm volatile ("svc %1" : "+r"(r0) : "I"(SVC_MEM_EXTFRAG) : "memory");
    return r0;
}

//actual kernel implementations (called from SVC handler)

int k_k_mem_init(void)
{
    if (g_inited) return RTX_ERR;

    uint32_t ebss_addr = (uint32_t)_ebss;

    // our stacks and TCBs take up a lot of space so _ebss might be lower than
    // we expect. just use 0x20009000 as a safe floor if _ebss is too low
    const uint32_t HEAP_START_FLOOR = 0x20009000u;

    if (ebss_addr > HEAP_START_FLOOR) {
        g_heap_start = ALIGN4_UP(ebss_addr);
    } else {
        g_heap_start = HEAP_START_FLOOR;
    }

    g_heap_end = HEAP_END_ADDR;

    // sanity check - make sure there's actually room for at least one block
    if (g_heap_end <= g_heap_start + HDR_SZ + 4u) return RTX_ERR;

    // set up the initial free block covering the whole heap
    BlockHeader *b = (BlockHeader *)g_heap_start;
    b->size = g_heap_end - g_heap_start - HDR_SZ;
    b->is_free = 1;
    b->owner = 0;
    b->_pad = 0;
    b->next_free = NULL;

    g_free_head = b;
    g_inited = 1;
    return RTX_OK;
}

void *k_k_mem_alloc(size_t size)
{
    if (!g_inited || size == 0) return NULL;

    // need to align to 4 bytes
    uint32_t need = ALIGN4_UP((uint32_t)size);

    // first fit - walk from the front of the free list
    BlockHeader *prev = NULL;
    BlockHeader *cur = g_free_head;

    while (cur != NULL) {
        if (cur->size >= need) break;
        prev = cur;
        cur = cur->next_free;
    }

    if (cur == NULL) return NULL; // nothing big enough, out of memory

    // split the block if there's enough leftover space to make it worth it
    // (needs room for a header + at least 4 bytes of payload)
    if (cur->size >= need + HDR_SZ + 4u) {
        BlockHeader *split = (BlockHeader *)((uint8_t *)cur + HDR_SZ + need);
        split->size = cur->size - need - HDR_SZ;
        split->is_free = 1;
        split->owner = 0;
        split->_pad = 0;
        split->next_free = cur->next_free;

        cur->size = need;

        // stitch split block into the free list where cur used to be
        if (prev == NULL) {
            g_free_head = split;
        } else {
            prev->next_free = split;
        }
    } else {
        // not worth splitting, just give them the whole thing
        if (prev == NULL) {
            g_free_head = cur->next_free;
        } else {
            prev->next_free = cur->next_free;
        }
    }

    cur->is_free = 0;
    cur->next_free = NULL;
    // track which task owns this block so we can enforce ownership on free
    // if kernel hasn't started yet, k_get_current_tid returns TID_NULL (0) which is fine
    cur->owner = (uint8_t)k_get_current_tid();

    return (void *)((uint8_t *)cur + HDR_SZ);
}

int k_k_mem_dealloc(void *ptr)
{
    if (!g_inited) return RTX_ERR;
    if (ptr == NULL) return RTX_OK; // freeing NULL is a no-op per spec

    BlockHeader *hdr = (BlockHeader *)((uint8_t *)ptr - HDR_SZ);

    // make sure the pointer actually points somewhere in our heap
    if ((uint32_t)hdr < g_heap_start || (uint32_t)hdr >= g_heap_end)
        return RTX_ERR;

    // can't free something that's already free
    if (hdr->is_free) return RTX_ERR;

    // only the task that allocated it can free it

    uint8_t caller = (uint8_t)k_get_current_tid();
    if (hdr->owner != caller) return RTX_ERR;

    hdr->is_free = 1;
    hdr->owner = 0;

    // coalescence section of th code
    // check right neighbour
    BlockHeader *nxt = phys_next(hdr);
    if (nxt != NULL && nxt->is_free) {
        fl_remove(nxt);
        hdr->size += HDR_SZ + nxt->size;
    }

    // now check if there's a free block immediately to the left of hdr have to walk the free list to find it
    BlockHeader *pred = NULL;
    BlockHeader *cur = g_free_head;
    while (cur != NULL && (uint32_t)cur < (uint32_t)hdr) {
        if ((uint32_t)cur + HDR_SZ + cur->size == (uint32_t)hdr) {
            pred = cur;
        } else {
            pred = NULL; // reset - this one isn't adjacent
        }
        cur = cur->next_free;
    }

    if (pred != NULL) {// merge hdr into pred in place (pred is already in the free list)
        pred->size += HDR_SZ + hdr->size;
    } else {// no left neighbour to merge with, just insert hdr into the free list
        fl_insert(hdr);
    }

    return RTX_OK;
}

int k_k_mem_count_extfrag(size_t size)
{
    if (!g_inited) return 0;

    // count free blocks whose total size (header + payload) is strictly less than `size per the spec, the header bytes count as free space too
    int count = 0;
    BlockHeader *cur = g_free_head;
    while (cur != NULL) {
        if (HDR_SZ + cur->size < (uint32_t)size) {
            count++;
        }
        cur = cur->next_free;
    }
    return count;
}
