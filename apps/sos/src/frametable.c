#include <sel4/sel4.h>
#include <device/mapping.h>
#include <device/vmem_layout.h>
#include <sync/mutex.h>
#include <cspace/cspace.h>
#include <limits.h>
#include <ut/ut.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>

#define FRAME_REGION_SIZE   (1ull << FRAME_SIZE_BITS)
#define NFRAMES             (FRAME_REGION_SIZE) / PAGE_SIZE
#define FADDR_TO_VADDR(faddr) (faddr + FRAME_VSTART)
#define VADDR_TO_FADDR(vaddr) (vaddr - FRAME_VSTART)
#define FADDR_TO_IDX(faddr) (faddr >> 4)

static sync_mutex_t frame_table_m;

struct frame_entry {
    seL4_CPtr cap;
    struct frame_entry * next_free;
    int ref_count;
    seL4_Word paddr;
};

typedef struct frame_entry frame_entry_t;

static frame_entry_t * free_list;
static frame_entry_t * frame_table;

// alloc a new physical page and map it to ith frame
static int frame_new_page(size_t k) {
    assert(frame_table);
    // alloc a physical page
    seL4_Word paddr = ut_alloc(seL4_PageBits);
    if (paddr == 0) {
        return ENOMEM;
    }
    seL4_CPtr cap;
    // retype it
    seL4_Error cerr = cspace_ut_retype_addr (
        paddr,
        seL4_ARM_SmallPageObject,
        seL4_PageBits,
        cur_cspace,
        &cap);
    if (cerr != seL4_NoError) {
        ut_free(paddr, seL4_PageBits);
        return EFAULT;
    }
    // map to correct address
    seL4_Word vaddr = FADDR_TO_VADDR(k*PAGE_SIZE);
    int err = map_page(cap, seL4_CapInitThreadPD, vaddr, seL4_AllRights, seL4_ARM_Default_VMAttributes);
    if (err) {
        ut_free(paddr, seL4_PageBits);
        cspace_delete_cap(cur_cspace, cap);
        return EFAULT;
    }
    frame_table[k].cap = cap;
    frame_table[k].paddr = paddr;
    return 0;
}

int frame_init(void) {
    assert(frame_table_m = sync_create_mutex());
    size_t frametable_sz = NFRAMES * sizeof(frame_entry_t);
    // allocate memory for storing frametable itself

    frame_table = (frame_entry_t *)FADDR_TO_VADDR(0);
    size_t i = 0;
    for (i = 0; i*PAGE_SIZE < frametable_sz; i++) {
        int err = frame_new_page(i);
        if (err)
            return err;
        frame_table[i].next_free = NULL;
    }
    free_list = &frame_table[i];
    for (; i < NFRAMES; i++) {
        if (i < NFRAMES-1)
            frame_table[i].next_free = &frame_table[i+1];
        else
            frame_table[i].next_free = NULL;
    }
    // TODO: Doesn't seem to move the frame table along?
    return 0;
}

seL4_Word frame_alloc(seL4_Word *vaddr) {
    // TODO: Don't crash when mem exhausted?
    assert(frame_table);
    // TODO: Check validity of vaddr

    // alloc a physical page
    seL4_Word paddr = ut_alloc(seL4_PageBits);
    if (paddr == 0) {
        printf("NO MEM\n");
        *vaddr = 0;
        return 0;
    }
    seL4_CPtr cap;
    // retype it
    seL4_Error cerr = cspace_ut_retype_addr(
        paddr,
        seL4_ARM_SmallPageObject,
        seL4_PageBits,
        cur_cspace,
        &cap);
    if (cerr != seL4_NoError) {
        *vaddr = 0;
        ut_free(paddr, seL4_PageBits);
        printf("cerr raised\n");
        return 0;
    }
    sync_acquire(frame_table_m);
    seL4_Word new_vaddr = VADDR_TO_FADDR((seL4_Word)free_list) * PAGE_SIZE;
    int err = map_page(cap, seL4_CapInitThreadPD, new_vaddr, seL4_AllRights, seL4_ARM_Default_VMAttributes);
    if (err) {
        printf("mapping fault for new_vaddr: %p\n", new_vaddr);
        ut_free(paddr, seL4_PageBits);
        printf("ut_free okay\n", new_vaddr);
        cspace_delete_cap(cur_cspace, cap);
        printf("cspace_delete_cap okay\n", new_vaddr);
        *vaddr = 0;
        return 0;
    }
    frame_entry_t *alloc_page = free_list;
    alloc_page->cap = cap;
    alloc_page->paddr = paddr;
    free_list = free_list->next_free;
    alloc_page->next_free = NULL;
    sync_release(frame_table_m);
    *vaddr = new_vaddr;
    printf("setting as: %p\n", (void*)alloc_page);
    seL4_Word faddr = FADDR_TO_IDX(VADDR_TO_FADDR((seL4_Word)alloc_page));
    printf("idx: %u\n", faddr);
    return faddr;
}

void frame_free(seL4_Word faddr) {
    // TODO: Don't crash when mem exhausted?
    assert(frame_table);
    // TODO: Check validity of vaddr
    sync_acquire(frame_table_m);
    frame_entry_t *cur_frame = &frame_table[faddr];
    // TODO: Unmap page
    cspace_delete_cap(cur_cspace, cur_frame->cap);
    ut_free(cur_frame->paddr, seL4_PageBits);
    cur_frame->next_free = free_list;
    free_list = cur_frame;
    sync_release(frame_table_m);
}

static void frame_test_1(void) {
    int i;
    printf("Starting test 1\n");
    /* Allocate 10 pages and make sure you can touch them all */
    for (i = 0; i < 20; i++) {
        /* Allocate a page */
        seL4_Word vaddr;
        frame_alloc(&vaddr);
        assert(vaddr);

        /* Test you can touch the page */
        *((unsigned*)vaddr) = 0x37;
        assert(*((unsigned*)vaddr) == 0x37);

        printf("Page #%d allocated at %p\n",  i, (void *)vaddr);
    }
    printf("Test 1 complete\n");
}

static void frame_test_2(void) {
    printf("Starting test 2\n");
    /* Test that you eventually run out of memory gracefully, and doesn't crash */
    for (;;) {
        /* Allocate a page */
        seL4_Word vaddr;
        frame_alloc(&vaddr);
        if (!vaddr) {
            printf("Out of memory!\n");
            break;
        }

        /* Test you can touch the page */
        *((unsigned*)vaddr) = 0x37;
        assert(*((unsigned*)vaddr) == 0x37);
    }
    printf("Test 2 complete\n");
}

static void frame_test_3(void) {
    printf("Starting test 3\n");
    /* Test that you never run out of memory if you always free frames. This loop should never finish */
    for (int i = 0;; i++) {
        /* Allocate a page */
        seL4_Word vaddr;
        seL4_Word page =  (seL4_Word)frame_alloc(&vaddr);
        assert(vaddr != 0);

        /* Test you can touch the page */
        *((unsigned*)vaddr) = 0x37;
        assert(*((unsigned*)vaddr) == 0x37);

        printf("Page #%d allocated at %p\n",  i, vaddr);
        assert(page > 0);
        frame_free(page);
    }
    printf("Test 3 complete\n");
}

void run_frame_tests(void) {
    frame_test_1();
    //frame_test_2();
    frame_test_3();
}
