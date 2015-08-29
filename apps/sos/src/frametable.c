#include <sel4/sel4.h>
#include <device/mapping.h>
#include <device/vmem_layout.h>
#include <cspace/cspace.h>
#include <limits.h>
#include <ut/ut.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include "frametable.h"

#define verbose 5
#include <log/debug.h>
#include <log/panic.h>

#define FRAME_REGION_SIZE   (1ull << FRAME_SIZE_BITS)
#define FADDR_TO_VADDR(faddr) (faddr + FRAME_VSTART)
#define VADDR_TO_FADDR(vaddr) (vaddr - FRAME_VSTART)

/* Minimum of two values. */
#define MIN(a,b) (((a)<(b))?(a):(b))

/* Maximum number of frames which will fit in our region */
#define MAX_FRAMES ((PROCESS_STACK_TOP - FRAME_VSTART - PAGE_SIZE) / PAGE_SIZE)

static unsigned nframes;

struct frame_entry {
    seL4_CPtr cap;
    struct frame_entry * next_free;
    seL4_Word paddr;
    size_t map_req_count;
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
 * Retrieve the cap corresponding to a frame
 */
seL4_CPtr frame_cap(seL4_Word vaddr) {
    seL4_Word idx = VADDR_TO_FADDR(vaddr) / PAGE_SIZE;
    assert(frame_table);
    conditional_panic(idx <= 0 || idx > nframes, "Cap does not exist\n");
    frame_entry_t *cur_frame = &frame_table[idx];
    return cur_frame->cap;
}

seL4_Word frame_paddr(seL4_Word vaddr) {
    seL4_Word idx = VADDR_TO_FADDR(vaddr) / PAGE_SIZE;
    assert(frame_table);
    conditional_panic(idx <= 0 || idx > nframes, "Cap does not exist\n");
    frame_entry_t *cur_frame = &frame_table[idx];
    return cur_frame->paddr;
}

/**
 * Map the frame at vaddr into SOS
 * @param vaddr the virtual address of the frame upon which to act
 * @return 0 on success, non-zero on failure
 */
int sos_map_frame(seL4_Word vaddr) {
    seL4_CPtr cap = frame_cap(vaddr);

    assert(vaddr < (PROCESS_STACK_BOTTOM - PAGE_SIZE));
    seL4_Word idx = VADDR_TO_FADDR(vaddr) / PAGE_SIZE;
    frame_entry_t *cur_frame = &frame_table[idx];
    cur_frame->map_req_count++;
    if (cur_frame->map_req_count > 1) {
        return 0;
    }
    int err = map_page(cap, seL4_CapInitThreadPD, vaddr, seL4_AllRights, seL4_ARM_Default_VMAttributes);
    if (err) {
        ERR("Unable to map page\n");
        ut_free(frame_paddr(vaddr), seL4_PageBits);
        cspace_delete_cap(cur_cspace, cap);
        return EINVAL;
    }
    return 0;
 }

/**
 * Unmap the frame at vaddr from SOS
 * @param vaddr the virtual address of the frame upon which to act
 * @return 0 on success, non-zero on failure
 */
int sos_unmap_frame(seL4_Word vaddr) {
    assert(vaddr < (PROCESS_STACK_TOP - PAGE_SIZE));
    seL4_Word idx = VADDR_TO_FADDR(vaddr) / PAGE_SIZE;
    frame_entry_t *cur_frame = &frame_table[idx];
    cur_frame->map_req_count--;
    if (!cur_frame->map_req_count == 0) {
        frame_free(vaddr);
        return 0;
    }
    return seL4_ARM_Page_Unmap(cur_frame->cap);
}

static int frame_map_page(unsigned idx) {
    assert(frame_table);
    assert(idx >= 0 && idx < nframes);

    // alloc a physical page
    seL4_Word paddr = ut_alloc(seL4_PageBits);
    if (paddr == 0) {
        ERR("[frametable] Out of memory\n");
        return ENOMEM;
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
        return EINVAL;
    }
    seL4_Word vaddr = FADDR_TO_VADDR(idx*PAGE_SIZE);
    assert((unsigned)vaddr < (PROCESS_STACK_TOP - PAGE_SIZE));
    int err = map_page(cap, seL4_CapInitThreadPD, vaddr, seL4_AllRights, seL4_ARM_Default_VMAttributes);
    if (err) {
        ERR("Unable to map page\n");
        ut_free(paddr, seL4_PageBits);
        cspace_delete_cap(cur_cspace, cap);
        return EINVAL;
    }
    memset((void*)vaddr, 0, PAGE_SIZE);
    frame_table[idx].cap = cap;
    frame_table[idx].paddr = paddr;
    frame_table[idx].map_req_count++;
    return 0;
}

/**
 * Initialise the frame table
 */
void frame_init(void) {
    set_num_frames();
    size_t frametable_sz = MIN(nframes, MAX_FRAMES) * sizeof(frame_entry_t);
    // allocate memory for storing frametable itself
    frame_table = (frame_entry_t *)FADDR_TO_VADDR(0);
    size_t i = 0;
    for (i = 0; i*PAGE_SIZE < frametable_sz; i++) {
        assert(frame_map_page(i) == 0);
        frame_table[i].next_free = NULL;
    }
    // Init next_free list
    assert(i > 0 && i < nframes);
    free_list = &frame_table[i];
    for (; i < nframes; i++) {
        if (i < nframes-1) {
            frame_table[i].next_free = &frame_table[i+1];
        }
        else {
            frame_table[i].next_free = NULL;
        }
        frame_table[i].map_req_count = 0;
    }
}

/**
 * Allocate a new frame
 * @param vaddr Pointer to the location the pointer will be provided
 * @return index of the frame in the table (faddr); 0 if failed to allocate
 */
seL4_Word frame_alloc(seL4_Word *vaddr) {
    assert(frame_table);
    if (!vaddr) {
        ERR("frame_alloc: passed null pointer\n");
        return 0;
    }
    if (!free_list) {
        ERR("[frametable] Free list empty\n");
        *vaddr = 0;
        return 0;
    }
    frame_entry_t* new_frame = free_list;
    free_list = free_list->next_free;
    unsigned idx = ((unsigned)new_frame-(unsigned)frame_table) / sizeof(frame_entry_t);
    //unsigned idx = new_frame - frame_table;
    int err = frame_map_page(idx);
    if (err) {
        ERR("[frametable] Failed to map page: err %d\n", err);
        *vaddr = 0;
        return 0;
    }
    new_frame->next_free = NULL;
    *vaddr = FADDR_TO_VADDR(idx*PAGE_SIZE);
    return *vaddr;
}

/**
 * Free the frame
 * @param vaddr Index of the frame to be removed
 * @return 0 on success, non-zero on failure
 */
int frame_free(seL4_Word vaddr) {
    seL4_Word idx = VADDR_TO_FADDR(vaddr) / PAGE_SIZE;
    assert(frame_table);
    if (idx <= 0 || idx > nframes) {
        ERR("frame_free: illegal faddr received\n");
        return EINVAL;
    }
    frame_entry_t *cur_frame = &frame_table[idx];
    if (cur_frame->map_req_count != 0) {
        return EPERM;
    }
    seL4_ARM_Page_Unmap(cur_frame->cap);
    cspace_err_t err = cspace_delete_cap(cur_cspace, cur_frame->cap);
    if (err != CSPACE_NOERROR) {
        ERR("frame_free: failed to delete CAP\n");
        return EINVAL;
    }
    ut_free(cur_frame->paddr, seL4_PageBits);
    cur_frame->next_free = free_list;
    free_list = cur_frame;
    return 0;
}