#include <sel4/sel4.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <errno.h>
#include <device/vmem_layout.h>
#include <ut/ut.h>

#include <device/mapping.h>
#include "frametable.h"
#include "addrspace.h"
#include <assert.h>

#define verbose 2
#include <log/debug.h>
#include <log/panic.h>

#define PT_BITS 10
#define PD_BITS 10

#define PT_SIZE (1ul << PT_BITS)
#define PD_SIZE (1ul << PD_BITS)
#define PAGE_ALIGN(a) (a & 0xfffff000)

#define RW_PAGE 0x3

#define PD_LOOKUP(vaddr) (vaddr >> (32ul - PD_BITS))
#define PT_LOOKUP(vaddr) ((vaddr << PD_BITS) >> (32ul - PT_BITS))

seL4_Word as_page_lookup(sos_addrspace_t *as, seL4_Word vaddr) {
    assert(as);
    seL4_Word pd_idx = PD_LOOKUP(vaddr);
    sos_pde_t *pde = as->pd[pd_idx];
    seL4_Word pt_idx = PT_LOOKUP(vaddr);
    if (pde) {
        return pde->pt[pt_idx];
    }
    return 0;
}

sos_region_t* as_vaddr_region(sos_addrspace_t *as, seL4_Word vaddr) {
    assert(as);
    sos_region_t *region = as->regions;
    while (region) {
        if (vaddr >= region->start && vaddr <= region->end) {
            return region;
        }
        region = region->next;
    }
    return NULL;
}

/**  ---  PAGE TABLE MAPPING  --- **/

static int
_proc_map_pagetable(sos_addrspace_t *as, seL4_Word pd_idx, seL4_Word vaddr) {
    int err;
    assert(pd_idx < PD_SIZE && pd_idx >= 0);
    sos_pde_t *pde = as->pd[pd_idx];

    /* Allocate a PT object */
    pde->sos_pt_addr = ut_alloc(seL4_PageTableBits);
    if(pde->sos_pt_addr == 0){
        ERR("PT ut_alloc failed\n");
        return 1;
    }

    /* Create the frame cap */
    err =  cspace_ut_retype_addr(pde->sos_pt_addr,
                                 seL4_ARM_PageTableObject,
                                 seL4_PageTableBits,
                                 cur_cspace,
                                 &pde->sos_pt_cap);
    if(err){
        ERR("retype failed\n");
        return 1;
    }

    /* Tell seL4 to map the PT in for us */
    err = seL4_ARM_PageTable_Map(pde->sos_pt_cap,
                                 as->sos_pd_cap,
                                 vaddr,
                                 seL4_ARM_Default_VMAttributes);
    if (err) {
        ERR("Mapping PT failed\n");
    }
    return 0;
}

static seL4_CPtr as_alloc_page(sos_addrspace_t *as, seL4_Word* sos_vaddr) {
    assert(as);

    // Create a frame
    frame_alloc(sos_vaddr);
    conditional_panic(*sos_vaddr == 0, "Unable to allocate memory from the SOS frametable\n");

    // Retrieve the Cap for the newly created frame
    seL4_CPtr fc = frame_cap(*sos_vaddr);
    assert(fc != seL4_CapNull);
    return fc;
}

static int as_map(sos_addrspace_t *as, seL4_Word vaddr, seL4_Word* sos_vaddr, seL4_CPtr fc) {
    int err;
    assert(PT_SIZE == 1024);
    assert(PD_SIZE == 1024);
    assert(PAGE_SIZE == 4096);
    // Copy the cap so we can map it into the process' PD
    seL4_CPtr proc_fc = cspace_copy_cap(cur_cspace,
                                        cur_cspace,
                                        fc,
                                        seL4_AllRights);
    assert(proc_fc != seL4_CapNull);
    seL4_Word pd_idx = PD_LOOKUP(vaddr);
    assert(pd_idx < PD_SIZE && pd_idx >= 0);
    if (as->pd[pd_idx] == NULL) {
        as->pd[pd_idx] = malloc(sizeof(sos_pde_t));
        conditional_panic(!as->pd[pd_idx], "Couldn't allocate for PT\n");
        as->pd[pd_idx]->pt = NULL;
    }
    conditional_panic(!as->pd[pd_idx], "Failed to allocate for PDE structure\n");

    // Lookup the permissions of the given vaddr
    sos_region_t *region = as_vaddr_region(as, vaddr);
    if (region == NULL) {
        ERR("No Region found for %u\n", vaddr);
        return EFAULT;
    }
    err = seL4_ARM_Page_Map(proc_fc, as->sos_pd_cap, PAGE_ALIGN(vaddr), region->perms,
                            seL4_ARM_Default_VMAttributes);

    if (err == seL4_FailedLookup) {
        err = _proc_map_pagetable(as, pd_idx, vaddr);
        conditional_panic(err, "Failed to map page table into PD");
        err = seL4_ARM_Page_Map(proc_fc, as->sos_pd_cap, PAGE_ALIGN(vaddr), region->perms,
                                seL4_ARM_Default_VMAttributes);
        conditional_panic(err, "2nd attempt to map page failed Failed to map page");
    }
    assert(!err);
    seL4_Word pt_idx = PT_LOOKUP(vaddr);
    assert(pt_idx < PT_SIZE && pt_idx >= 0);
    if (as->pd[pd_idx]->pt == NULL) {
        err = (int)frame_alloc((seL4_Word*)&as->pd[pd_idx]->pt);
        sos_map_frame((seL4_Word)as->pd[pd_idx]->pt);
        if (err == 0) {
            return ENOMEM;
        }
    }
    as->pd[pd_idx]->pt[pt_idx] = *sos_vaddr;
    return 0;
}

int as_map_page(sos_addrspace_t *as, seL4_Word vaddr, seL4_Word* sos_vaddr) {
    seL4_CPtr cap;
    cap = as_alloc_page(as, sos_vaddr);
    return as_map(as, vaddr, sos_vaddr, cap);
}

/**  ---  REGION HANDLING  --- **/

/**
 * Create a new region
 * @param start the absolute start position of the region
 * @param end the absolute end position of the region
 * @param perms the permissions that should be offered to the region
 */
sos_region_t* as_region_create(sos_addrspace_t *as, seL4_Word start, seL4_Word end, int perms) {
    assert(as);
    assert(perms);
    assert(start);
    assert(end);
    sos_region_t *new_region;
    new_region = malloc(sizeof(sos_region_t));
    conditional_panic(!new_region, "Unable to create new region for process\n");
    new_region->start = start;
    new_region->end = end;
    new_region->perms = (unsigned)perms;
    new_region->next = NULL;

    if (as->regions == NULL) {
        as->regions = new_region;
        return new_region;
    }

    sos_region_t *last_region;
    for(last_region = as->regions; last_region->next != NULL; last_region = last_region->next);

    last_region->next = new_region;
    return new_region;
}

/**
 * Create statically positioned regions
 * @return error code or 0 for success
 */
static int init_regions(sos_addrspace_t *as) {
    assert(as);

    seL4_Word heap_start = 0 + PAGE_SIZE;
    sos_region_t* cur_region;

    for(cur_region = as->regions; cur_region != NULL;
        cur_region = cur_region->next) {
        if (cur_region->end > heap_start) {
            heap_start = cur_region->end + PAGE_SIZE;
        }
    }

    heap_start = PAGE_ALIGN(heap_start);
    // Map stack as non-exec
    as->heap_region = as_region_create(as, heap_start, heap_start, RW_PAGE);
    as->ipc_region = as_region_create(as, PROCESS_IPC_BUFFER, PROCESS_IPC_BUFFER + (1 << seL4_PageBits), RW_PAGE);
    as->stack_region = as_region_create(as, PROCESS_STACK_BOTTOM, PROCESS_STACK_TOP, RW_PAGE);

    if (as->stack_region == NULL || as->ipc_region == NULL || as->heap_region == NULL) {
        ERR("Default regions setup failed\n");
        return ENOMEM;
    }

    return 0;
}

seL4_Word brk(sos_addrspace_t *as, uintptr_t newbrk) {
    if (!newbrk) {
        return as->heap_region->end;
    } else if (newbrk < as->stack_region->start && newbrk >= as->heap_region->start) {
        return (as->heap_region->end = newbrk);
    }
    return 0;
}

/**  ---  ADDRESS SPACE INIT  --- **/

/**
 * Map the default regions (ipc_buf, stack) into the PT
 */
void init_essential_regions(sos_addrspace_t* as) {
    int err;
    err = init_regions(as);
    conditional_panic(err, "CREATING REGIONS FAILED\n");
    seL4_CPtr ipc_cap = frame_cap(as->sos_ipc_buf_addr);
    as_map(as, PROCESS_IPC_BUFFER, &as->sos_ipc_buf_addr, ipc_cap);
    as_map_page(as, PROCESS_STACK_TOP - (1 << seL4_PageBits), &as->sos_stack_addr);
}

/**
 * Create a new address space
 */
sos_addrspace_t* as_create(void) {
    int err;
    sos_addrspace_t *as = malloc(sizeof(sos_addrspace_t));
    conditional_panic(!as, "No memory for address space");
    as->sos_pd_addr = ut_alloc(seL4_PageDirBits);
    conditional_panic(!as->sos_pd_addr, "No memory for new Page Directory");
    err = cspace_ut_retype_addr(as->sos_pd_addr,
                                seL4_ARM_PageDirectoryObject,
                                seL4_PageDirBits,
                                cur_cspace,
                                &as->sos_pd_cap);
    conditional_panic(err, "Failed to allocate page directory cap for client");
    as->regions = NULL;
    // Create the page directory
    err = (int)frame_alloc((seL4_Word*)&as->pd);
    conditional_panic(!err, "Unable to get frame for PD!\n");
    sos_map_frame((seL4_Word)as->pd);

    as_alloc_page(as, &as->sos_ipc_buf_addr);
    return as;
}
