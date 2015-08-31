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
#define PAGE_ALIGN_UP(a) ((a + PAGE_SIZE - 1) & 0xfffff000)

#define RW_PAGE (0x3)

#define PD_LOOKUP(vaddr) (vaddr >> (32ul - PD_BITS))
#define PT_LOOKUP(vaddr) ((vaddr << PD_BITS) >> (32ul - PT_BITS))

/**
 * Lookup a sos vaddr (SOS frametable address) given a client vaddr
 * @param as address space
 * @param vaddr client virtual address
 * @return sos vaddr corresponding to the vaddr, or 0 in the event of failure
 */
sos_vaddr as_lookup_sos_vaddr(sos_addrspace_t *as, client_vaddr vaddr) {
    assert(as);
    seL4_Word pd_idx = PD_LOOKUP(vaddr);
    seL4_Word pt_idx = PT_LOOKUP(vaddr);
    if (as->pd[pd_idx]) {
        return (as->pd[pd_idx][pt_idx] + (0x00000fff & vaddr));
    }
    return 0;
}

/**
 * Lookup a region given a vaddr
 * @param as the address space
 * @param vaddr the virtual address whose region you're seeking
 * @return the region for that address space
 */
sos_region_t* as_vaddr_region(sos_addrspace_t *as, client_vaddr vaddr) {
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

/**
 * Create a new page table in the given page directory
 */
static int
_proc_map_pagetable(sos_addrspace_t *as, seL4_Word pd_idx, client_vaddr vaddr) {
    int err;
    assert(pd_idx < PD_SIZE && pd_idx >= 0);
    kpt_t *new_pt = malloc(sizeof(kpt_t));

    new_pt->addr = ut_alloc(seL4_PageTableBits);
    if(new_pt->addr == 0){
        ERR("PT ut_alloc failed\n");
        return 1;
    }

    /* Create the frame cap */
    err =  cspace_ut_retype_addr(new_pt->addr,
                                 seL4_ARM_PageTableObject,
                                 seL4_PageTableBits,
                                 cur_cspace,
                                 &new_pt->cap);
    if(err){
        ERR("retype failed\n");
        return 1;
    }

    /* Tell seL4 to map the PT in for us */
    err = seL4_ARM_PageTable_Map(new_pt->cap,
                                 as->sos_pd_cap,
                                 vaddr,
                                 seL4_ARM_Default_VMAttributes);
    if (err) {
        ERR("Mapping PT failed\n");
    }

    new_pt->next = as->kpts;
    as->kpts = new_pt;
    return 0;
}

/**
 * Allocate a new frame
 */
static seL4_CPtr as_alloc_page(sos_addrspace_t *as, seL4_Word* sos_vaddr) {
    assert(as);
    // Create a frame
    frame_alloc(sos_vaddr);
    conditional_panic(*sos_vaddr == 0, "Unable to allocate memory from the SOS frametable\n");
    //sos_unmap_frame(*sos_vaddr);

    // Retrieve the Cap for the newly created frame
    seL4_CPtr fc = frame_cap(*sos_vaddr);
    assert(fc != seL4_CapNull);
    return fc;
}

/**
 * Map a page into the address space
 * @param as the address space
 * @param vaddr the location to map the page
 * @param sos_vaddr the address of the corresponding frame
 * @param fc the frame cap
 * @param rights the rights to assign to the page
 * @return 0 on success, non-zero on failure.
 */
static int as_map_page(sos_addrspace_t *as, seL4_Word vaddr, seL4_Word* sos_vaddr, seL4_CPtr fc, seL4_CapRights rights) {
    int err;
    // Copy the cap so we can map it into the process' PD
    seL4_CPtr proc_fc = cspace_copy_cap(cur_cspace,
                                        cur_cspace,
                                        fc,
                                        seL4_AllRights);
    assert(proc_fc != seL4_CapNull);
    seL4_Word pd_idx = PD_LOOKUP(vaddr);

    err = seL4_ARM_Page_Map(proc_fc, as->sos_pd_cap, PAGE_ALIGN(vaddr), rights,
                            seL4_ARM_Default_VMAttributes);

    if (err == seL4_FailedLookup) {
        err = _proc_map_pagetable(as, pd_idx, vaddr);
        conditional_panic(err, "Failed to map page table into PD");
        err = seL4_ARM_Page_Map(proc_fc, as->sos_pd_cap, PAGE_ALIGN(vaddr), rights,
                                seL4_ARM_Default_VMAttributes);
        conditional_panic(err, "2nd attempt to map page failed Failed to map page");
    }
    assert(!err);
    seL4_Word pt_idx = PT_LOOKUP(vaddr);
    assert(pt_idx < PT_SIZE && pt_idx >= 0);
    if (as->pd[pd_idx] == NULL) {
        err = (int)frame_alloc((seL4_Word*)&as->pd[pd_idx]);
        if (err == 0) {
            return ENOMEM;
        }
    }
    as->pd[pd_idx][pt_idx] = *sos_vaddr;
    return 0;
}

/**
 * Create and map a new page into the address space
 * @param as the address space to act upon
 * @param vaddr the address in `as' to map the page
 * @param rights the permissions which should be set (as per seL4_CapRights) for the new mapping.  Only Read and Write are respected.
 * @return 0 on success, non-zero otherwise.
 */
int as_create_page(sos_addrspace_t *as, seL4_Word vaddr, seL4_CapRights rights) {
    seL4_CPtr cap;
    seL4_Word sos_vaddr;
    cap = as_alloc_page(as, &sos_vaddr);
    return as_map_page(as, vaddr, &sos_vaddr, cap, rights);
}

/**  ---  REGION HANDLING  --- **/

/**
 * Create a new region
 * @param start the absolute start position of the region
 * @param end the absolute end position of the region
 * @param rights the permissions that should be offered to the region
 * @return the newly created region
 */
sos_region_t* as_region_create(sos_addrspace_t *as, seL4_Word start, seL4_Word end, int rights) {
    assert(as);
    sos_region_t *new_region;
    // Quick check to make sure we don't overlap another region.  Does not
    // detect should we totally mask an existing region, though does offer
    // some protection...
    if (as_vaddr_region(as, PAGE_ALIGN(start)) != 0 ||
        as_vaddr_region(as, PAGE_ALIGN_UP(end)) != 0) {
        return NULL;
    }
    new_region = malloc(sizeof(sos_region_t));
    conditional_panic(!new_region, "Unable to create new region for process\n");
    new_region->start = PAGE_ALIGN(start);
    new_region->end = PAGE_ALIGN_UP(end);
    new_region->rights = (seL4_CapRights)rights;
    new_region->next = as->regions;
    as->regions = new_region;
    return new_region;
}

/**
 * Create statically positioned regions
 * @param as the address space to act upon
 * @return error code or 0 for success
 */
static int create_non_segment_regions(sos_addrspace_t *as) {
    assert(as);
    assert(as->regions);
    seL4_Word heap_start = 0;
    sos_region_t* cur_region;
    for(cur_region = as->regions; cur_region != NULL;
        cur_region = cur_region->next) {
        if (cur_region->end > heap_start) {
            heap_start = cur_region->end + PAGE_SIZE;
        }
    }

    heap_start = PAGE_ALIGN(heap_start);
    sos_region_t* ipc;
    as->heap_region = as_region_create(as, heap_start, heap_start, RW_PAGE);
    as->stack_region = as_region_create(as, PROCESS_STACK_BOTTOM, PROCESS_STACK_TOP, RW_PAGE);
    ipc = as_region_create(as, PROCESS_IPC_BUFFER, PROCESS_IPC_BUFFER + PAGE_SIZE, RW_PAGE);

    if (as->stack_region == NULL || ipc == NULL || as->heap_region == NULL) {
        ERR("Default regions setup failed\n");
        return ENOMEM;
    }

    return 0;
}

/**
 * Alter the heap brk point, or if passed a newbrk of zero, return the current brk
 * @param as the address space to act upon
 * @param newbrk the new absolute ending position of the brk
 * @return the address of the brk set. Or 0 in the event of failure.
 */
client_vaddr sos_brk(sos_addrspace_t *as, uintptr_t newbrk) {
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
 * Must be called following the initialisation of the process' TCB
 * @param as Address space
 */
void as_activate(sos_addrspace_t* as) {
    int err;
    err = create_non_segment_regions(as);
    conditional_panic(err, "CREATING REGIONS FAILED\n");
    seL4_CPtr ipc_cap = frame_cap(as->sos_ipc_buf_addr);
    sos_region_t *region = as_vaddr_region(as, PROCESS_IPC_BUFFER);
    as_map_page(as, PROCESS_IPC_BUFFER, &as->sos_ipc_buf_addr, ipc_cap, region->rights);
}

/**
 * Create a new address space
 * Should be called prior to the initialisation of a new process' TCB
 * @return pointer to the newly created address space (vspace)
 */
sos_addrspace_t* as_create(void) {
    int err;
    sos_addrspace_t *as = malloc(sizeof(sos_addrspace_t));
    conditional_panic(!as, "No memory for address space");
    memset(as, 0, sizeof(sos_addrspace_t));
    as->sos_pd_addr = ut_alloc(seL4_PageDirBits);
    conditional_panic(!as->sos_pd_addr, "No memory for new Page Directory");
    err = cspace_ut_retype_addr(as->sos_pd_addr,
                                seL4_ARM_PageDirectoryObject,
                                seL4_PageDirBits,
                                cur_cspace,
                                &as->sos_pd_cap);
    conditional_panic(err, "Failed to allocate page directory cap for client");
    // Create the page directory
    err = (int)frame_alloc((seL4_Word*)&as->pd);
    conditional_panic(!err, "Unable to get frame for PD!\n");
    as_alloc_page(as, &as->sos_ipc_buf_addr);
    return as;
}
