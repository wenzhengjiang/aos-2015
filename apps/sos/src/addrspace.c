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
#include "swap.h"
#include <assert.h>

#define verbose 5
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

static inline unsigned CONST umin(unsigned a, unsigned b) {
    return (a < b) ? a : b;
}

pte_t* as_lookup_pte(sos_addrspace_t *as, client_vaddr vaddr) {
    assert(as);
    seL4_Word pd_idx = PD_LOOKUP(vaddr);
    seL4_Word pt_idx = PT_LOOKUP(vaddr);
    if (as->pd[pd_idx] && as->pd[pd_idx][pt_idx]) {
        return (as->pd[pd_idx][pt_idx]);
    }
    return NULL;
}

bool as_page_exists(sos_addrspace_t *as, client_vaddr vaddr) {
    assert(as);
    pte_t* pte = as_lookup_pte(as, vaddr);
    if (pte == NULL) {
        return false;
    }
    return true;
}

/**
 * Lookup a sos vaddr (SOS frametable address) given a client vaddr
 * @param as address space
 * @param vaddr client virtual address
 * @return sos_vaddr corresponding to the vaddr, or 0 in the event of failure
 */
sos_vaddr as_lookup_sos_vaddr(sos_addrspace_t *as, client_vaddr vaddr) {
    assert(as);
    pte_t* pte = as_lookup_pte(as, vaddr);
    if (pte == NULL) {
        return 0;
    }
    return (LOAD_PAGE(pte->addr) + (0x00000fff & vaddr));
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
        if (vaddr >= region->start && vaddr < region->end) {
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


/** Address space destruction **/
static void as_free_region(sos_addrspace_t *as) {
    sos_region_t *reg;
    dprintf(3, "[AS] freeing regions\n");
    for (reg = as->regions; as->regions != NULL; reg = reg->next) {
        reg = as->regions->next;
        free(as->regions);
        as->regions = reg;
    }
    dprintf(4, "[AS] regions free'd\n");
}

static void as_free_kpts(sos_addrspace_t *as) {
    kpt_t *kpt;
    dprintf(3, "[AS] Freeing KPTs\n");
    for (kpt = as->kpts; as->kpts != NULL; kpt = kpt->next) {
        kpt = as->kpts->next;
        cspace_revoke_cap(cur_cspace, as->kpts->cap);
        cspace_err_t err = cspace_delete_cap(cur_cspace, as->kpts->cap);
        if (err != CSPACE_NOERROR) {
            ERR("[AS]: failed to delete kpt cap\n");
        }
        ut_free(as->kpts->addr, seL4_PageTableBits);
        free(as->kpts);
        as->kpts = kpt;
    }
    dprintf(4, "[AS] KPTs free'd\n");
}

static void as_free_ptes(sos_addrspace_t *as) {
    pte_t *pt;
    dprintf(3, "[AS] Freeing PTEs\n");
    sos_unmap_frame(as->sos_ipc_buf_addr);
    as->repllist_tail->next = NULL;
    for (pt = as->repllist_head; as->repllist_head != NULL; pt = pt->next) {
        pt = as->repllist_head->next;
        if (as->repllist_head->swapd) {
            // TODO: Make sure that the page cap is dealt with in this scenario
            dprintf(4, "[AS] freeing swap\n");
            swap_free(LOAD_PAGE(as->repllist_head->addr));
        } else {
            dprintf(4, "[AS] freeing frame\n");
            assert(as->repllist_head->page_cap != seL4_CapNull);
            cspace_revoke_cap(cur_cspace, as->repllist_head->page_cap);
            cspace_err_t err = cspace_delete_cap(cur_cspace, as->repllist_head->page_cap);
            if (err != CSPACE_NOERROR) {
                ERR("[AS]: failed to delete page cap\n");
            }
            printf("Unmapping\n");
            sos_unmap_frame(LOAD_PAGE(as->repllist_head->addr));
        }
        free(as->repllist_head);
        as->repllist_head = pt;
    }
    dprintf(4, "[AS] PTEs freed\n");
}

static void as_free_pd(sos_addrspace_t *as) {
    int max_pt = (1 << PD_BITS);
    dprintf(3, "[AS] freeing PD\n");
    for (int i = 0; i < max_pt; i++) {
        if (as->pd[i] != NULL) {
            sos_unmap_frame((seL4_Word)as->pd[i]);
        }
    }
    sos_unmap_frame((seL4_Word)as->pd);
    dprintf(4, "[AS] PD free'd\n");
}

void as_free(sos_addrspace_t *as) {
    dprintf(3, "[AS] Freeing address space\n");
    assert(as);
    as_free_region(as);
    as_free_kpts(as);
    as_free_ptes(as);
    as_free_pd(as);
    free(as);
    dprintf(4, "[AS] address space free'd\n");
}

/**
 * Allocate a new frame
 */
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

/**
 * Map a page into the address space
 * @param as the address space
 * @param vaddr the location to map the page
 * @param fc the frame cap
 * @param rights the rights to assign to the page
 * @return 0 on success, non-zero on failure.
 */
static int as_map_page(sos_addrspace_t *as, seL4_Word vaddr, seL4_CPtr fc, seL4_CapRights rights) {
    dprintf(0, "as_map_page");
    int err;
    unsigned pt_idx = PT_LOOKUP(vaddr);

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
    as->pd[pd_idx][pt_idx]->page_cap = proc_fc;
    as->pd[pd_idx][pt_idx]->refd = true;
    as->pd[pd_idx][pt_idx]->valid = true;
    return 0;
}

int as_add_page(sos_addrspace_t *as, client_vaddr vaddr, sos_vaddr sos_vaddr) {
    dprintf(0, "as_add_page");
    int err;
    seL4_Word pd_idx = PD_LOOKUP(vaddr);
    seL4_Word pt_idx = PT_LOOKUP(vaddr);
    assert(pt_idx < PT_SIZE && pt_idx >= 0);
    if (as->pd[pd_idx] == NULL) {
        err = (int)frame_alloc((seL4_Word*)&as->pd[pd_idx]);
        if (err == 0) {
            return ENOMEM;
        }
    }
    as->pd[pd_idx][pt_idx] = malloc(sizeof(pte_t));
    pte_t* pt = as->pd[pd_idx][pt_idx];
    if (pt == NULL) {
        return ENOMEM;
    }
    assert(sos_vaddr != 0);
    pt->refd = false;
    pt->valid = true;
    pt->swapd = false;
    pt->addr = SAVE_PAGE(sos_vaddr);

    if (as->repllist_tail == NULL) {
        assert(as->repllist_head == NULL);
        as->repllist_head = pt;
    } else {
        as->repllist_tail->next = pt;
    }
    as->repllist_tail = pt;
    pt->next = as->repllist_head;
    dprintf(0, "as_add_page complete");
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

    dprintf(2, "as_create_page\n");
    seL4_CPtr cap;
    seL4_Word sos_vaddr;
    cap = as_alloc_page(as, &sos_vaddr);
    int err = as_add_page(as, vaddr, sos_vaddr);
    if (err) {
        return err;
    }
    return as_map_page(as, vaddr, cap, rights);
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
 * Check whether a page has been referenced between pgae replacement attempts
 * @param as address space
 * @param vaddr client virtual address
 * @return bool indicating whether the page has been referenced
 */
bool is_referenced(sos_addrspace_t *as, client_vaddr vaddr) {
    assert(as);
    pte_t* pte = as_lookup_pte(as, vaddr);
    if (pte == NULL) {
        return false;
    }
    return pte->refd;
}

void as_reference_page(sos_addrspace_t *as, client_vaddr vaddr, seL4_CapRights rights) {
    pte_t* pte = as_lookup_pte(as, vaddr);
    if (pte == NULL) {
        assert(!"Page does not exist to be mapped");
    }
    seL4_CPtr cap = frame_cap(LOAD_PAGE(pte->addr));
    assert(cap != seL4_CapNull);
    as_map_page(as, vaddr, cap, rights);
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
    as_add_page(as, PROCESS_IPC_BUFFER, as->sos_ipc_buf_addr);
}

/**
 * Create a new address space
 * Should be called prior to the initialisation of a new process' TCB
 * @return pointer to the newly created address space (vspace)
 */
sos_addrspace_t* as_create(void) {
    int err;
    dprintf(3, "[AS] as_create\n");
    sos_addrspace_t *as = malloc(sizeof(sos_addrspace_t));
    conditional_panic(!as, "No memory for address space");
    memset(as, 0, sizeof(sos_addrspace_t));
    printf("finished memset\n");
    as->sos_pd_addr = ut_alloc(seL4_PageDirBits);
    conditional_panic(!as->sos_pd_addr, "No memory for new Page Directory");
    printf("cspace_ut_retyping\n");
    err = cspace_ut_retype_addr(as->sos_pd_addr,
                                seL4_ARM_PageDirectoryObject,
                                seL4_PageDirBits,
                                cur_cspace,
                                &as->sos_pd_cap);
    conditional_panic(err, "Failed to allocate page directory cap for client");
    // Create the page directory
    printf("Allocating new frame for PD\n");
    err = (int)frame_alloc((seL4_Word*)&as->pd);
    conditional_panic(!err, "Unable to get frame for PD!\n");
    printf("allocating page for buf addr\n");
    as_alloc_page(as, &as->sos_ipc_buf_addr);
    dprintf(3, "[AS] as_create success\n");
    return as;
}
