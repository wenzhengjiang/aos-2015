#include <sel4/sel4.h>
#include <device/mapping.h>
#include <device/vmem_layout.h>
#include <sync/mutex.h>
#include <cspace/cspace.h>
#include <limits.h>
#include <ut/ut.h>
#include <sys/debug.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>

#define verbose 5
#define FRAME_REGION_SIZE   (1ull << FRAME_SIZE_BITS)
#define NFRAMES             (FRAME_REGION_SIZE) / PAGE_SIZE
#define FADDR_TO_VADDR(faddr) (faddr + FRAME_VSTART)
#define VADDR_TO_FADDR(vaddr) (vaddr - FRAME_VSTART)
#define FADDR_TO_IDX(faddr) (faddr >> 4)

int nframes;

struct frame_entry {
    seL4_CPtr cap;
    struct frame_entry * next_free;
    int ref_count;
    seL4_Word paddr;
};

typedef struct frame_entry frame_entry_t;

static frame_entry_t * free_list;
static frame_entry_t * frame_table;

static void set_num_frames(void) {
    seL4_Word low,high;
    ut_find_memory(&low,&high);
    nframes = (high - low) / PAGE_SIZE;
}

/**
 * Allocate a new frame
 * @param vaddr Pointer to the location the pointer will be provided
 * @return index of the frame in the table (faddr)
 */
seL4_Word frame_alloc(seL4_Word *vaddr) {
    assert(frame_table);
    // alloc a physical page
    seL4_Word paddr = ut_alloc(seL4_PageBits);
    if (paddr == 0) {
        ERR("Out of memory\n");
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
        ERR("Unable to retype address\n");
        ut_free(paddr, seL4_PageBits);
        *vaddr = 0;
        return 0;
    }
    seL4_Word new_vaddr = VADDR_TO_FADDR((seL4_Word)free_list) * PAGE_SIZE;
    int err = map_page(cap, seL4_CapInitThreadPD, new_vaddr, seL4_AllRights, seL4_ARM_Default_VMAttributes);
    if (err) {
        ERR("Unable to map page\n");
        ut_free(paddr, seL4_PageBits);
        cspace_delete_cap(cur_cspace, cap);
        *vaddr = 0;
        return 0;
    }
    frame_entry_t *alloc_page = free_list;
    alloc_page->cap = cap;
    alloc_page->paddr = paddr;
    free_list = free_list->next_free;
    alloc_page->next_free = NULL;
    *vaddr = new_vaddr;
    seL4_Word faddr = FADDR_TO_IDX(VADDR_TO_FADDR((seL4_Word)alloc_page));
     return faddr;
}

/**
 * Initialise the frame table
 */
void frame_init(void) {
    set_num_frames();
    size_t frametable_sz = nframes * sizeof(frame_entry_t);
    seL4_Word discard;
    // allocate memory for storing frametable itself
    frame_table = (frame_entry_t *)FADDR_TO_VADDR(0);
    size_t i = 0;
    for (i = 0; i*PAGE_SIZE < frametable_sz; i++) {
        frame_alloc(&discard);
        assert(discard);
        frame_table[i].next_free = NULL;
    }
}

/**
 * Free the frame
 * @param faddr Index of the frame to be removed
 */
void frame_free(seL4_Word faddr) {
    assert(frame_table);
    if (faddr < 0 || faddr > nframes) {
        ERR("frame_free: illegal faddr received\n");
        return;
    }
    frame_entry_t *cur_frame = &frame_table[faddr];
    seL4_ARM_Page_Unmap(cur_frame->cap);
    cspace_err_t err = cspace_delete_cap(cur_cspace, cur_frame->cap);
    if (err != CSPACE_NOERROR) {
        ERR("frame_free: failed to delete CAP\n");
        return;
    }
    ut_free(cur_frame->paddr, seL4_PageBits);
    cur_frame->next_free = free_list;
    free_list = cur_frame;
}

static void frame_test_1(void) {
    int i;
    printf("Starting test 1\n");
    /* Allocate 10 pages and make sure you can touch them all */
    for (i = 0; i < 10; i++) {
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

void test_frametable(void) {
    frame_test_1();
    //frame_test_2();
    frame_test_3();
}
