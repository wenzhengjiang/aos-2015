#include <sel4/sel4.h>
#include <device/mapping.h>
#include <device/vmem_layout.h>
#include <cspace/cspace.h>
#include <limits.h>
#include <ut/ut.h>
#include <errno.h>
#include <stdlib.h>

#define FRAME_REGION_SIZE   (1ull << FRAME_SIZE_BITS)
#define NFRAMES             (FRAME_REGION_SIZE) / PAGE_SIZE
#define FADDR_TO_VADDR(faddr) (faddr + FRAME_VSTART)

struct frame_entry {
    seL4_CPtr cap;
    struct frame_entry * next_free;
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
    return 0;
}

int frame_init(void) {
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
    return 0;
}
