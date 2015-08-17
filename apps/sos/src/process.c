/** process.c --- Process management **/

#include <sel4/sel4.h>
#include <stdlib.h>
#include <limits.h>
#include <device/vmem_layout.h>
#include <errno.h>
#include <assert.h>
#include <ut/ut.h>
#include <device/mapping.h>

#include "process.h"
#include "frametable.h"

#define verbose 5
#include <sys/debug.h>
#include <sys/panic.h>

/* This is the index where a clients syscall enpoint will
 * be stored in the clients cspace. */
#define USER_EP_CAP          (1)
#define TEST_PRIORITY         (0)
#define TEST_EP_BADGE         (101)

#define PT_MASK (0x000ff000)

sos_proc_t test_proc;
sos_proc_t *curproc = &test_proc;

static sos_region_t*
_lookup_region(sos_proc_t *proc, seL4_Word vaddr) {
    sos_region_t *region = proc->vspace.regions;
    while (region && vaddr >= region->start && vaddr <= region->end) {
        region = region->next;
    }
    return region;
}

static int
_proc_map_pagetable(seL4_ARM_PageDirectory pd, sos_pde_t *pt, seL4_Word vaddr) {
    seL4_Word pt_addr;
    int err;

    /* Allocate a PT object */
    pt_addr = ut_alloc(seL4_PageTableBits);
    if(pt_addr == 0){
        ERR("PT ut_alloc failed\n");
        return !0;
    }
    /* Create the frame cap */
    err =  cspace_ut_retype_addr(pt_addr,
                                 seL4_ARM_PageTableObject,
                                 seL4_PageTableBits,
                                 cur_cspace,
                                 &pt->pt_cap);
    if(err){
        ERR("retype failed\n");
        return !0;
    }
    /* Tell seL4 to map the PT in for us */
    err = seL4_ARM_PageTable_Map(pt->pt_cap,
                                 pd,
                                 vaddr,
                                 seL4_ARM_Default_VMAttributes);
    if (err) {
        ERR("Mapping PT failed\n");
    }
    return err;
}

seL4_Word pt_lookup(sos_proc_t *proc, seL4_Word vaddr) {
    seL4_Word pd_idx = (vaddr >> 20);
    sos_pde_t *pde = &proc->vspace.pde[pd_idx];
    printf("pde: %x\n", (int)pde);
    seL4_Word pt_idx = ((vaddr & PT_MASK) >> 12);
    printf("pt_idx: %u\n", pt_idx);
    return pde->pt[pt_idx];
}

int
process_map_page(sos_proc_t *proc, seL4_Word vaddr) {
    int err = 0;
    assert(proc);
    assert(vaddr);
    // Lookup the permissions of the given vaddr
    sos_region_t *region = _lookup_region(proc, vaddr);
    if (region == NULL) {
        WARN("No Region found for %u\n", vaddr);
        return EFAULT;
    }

    // Create a frame
    seL4_Word temp_vaddr = vaddr;
    seL4_Word faddr = frame_alloc(&temp_vaddr);
    if (faddr == 0) {
        WARN("Frame alloc failed\n");
        return ENOMEM;
    }
 
    printf("Mapping vaddr: %x\n", vaddr);
    printf("Mapping vaddr: %u\n", vaddr);
    seL4_CPtr fc = frame_cap(faddr);
    assert(fc != seL4_CapNull);
    // Copy the cap into the process' cspace
    seL4_CPtr proc_fc = cspace_copy_cap(cur_cspace,
                                        cur_cspace,
                                        fc,
                                        seL4_AllRights);
    assert(proc_fc != seL4_CapNull);
    // Get the PDE using the upper 12-bits for PT
    seL4_Word pd_idx = (vaddr >> 20);
    sos_pde_t *pde = &proc->vspace.pde[pd_idx];
    printf("pde: %x\n", (int)pde);
    if (pde->pt_cap == seL4_CapNull) {
        printf("mapping!\n");
        _proc_map_pagetable(proc->vspace.pd, pde, vaddr);
        //pde->pt = ut_alloc(seL4_PageBits);
        //err =  cspace_ut_retype_addr(pde->pt,
        //                             seL4_ARM_SmallPageObject,
        //                             seL4_PageBits,
        //                             cur_cspace,
        //                             &pde->sos_pt_cap);
    }
    printf("mapping vaddr %x into pt...\n", vaddr);
    err = seL4_ARM_Page_Map(proc_fc, proc->vspace.pd, vaddr, seL4_AllRights,
                            seL4_ARM_Default_VMAttributes);
    if (err) {
        ERR("Mapping proc_fc failed: %d\n", err);
    }
    // TODO: Map in proc_fc
    seL4_Word pt_idx = ((vaddr & PT_MASK) >> 12);
    printf("pt_idx: %u\n", pt_idx);
    pde->pt[pt_idx] = faddr;

    return err;
}

/**
 * Create a region for the stack
 * @return pointer to the new stack region
 */
static sos_region_t* init_stack_region(void) {
    sos_region_t* region;
    region = (sos_region_t*)malloc(sizeof(sos_region_t));
    if (region == NULL) {
        return NULL;
    }
    region->start = PROCESS_STACK_TOP - (1 << seL4_PageBits);
    region->end = PROCESS_STACK_TOP;
    region->perms = seL4_AllRights;
    return region;
}

/**
 * Create a region for the IPC buffer
 * @return pointer to the new IPC Buffer region
 */
static sos_region_t* init_ipc_buf_region(void) {
    sos_region_t* region;
    region = (sos_region_t*)malloc(sizeof(sos_region_t));
    if (region == NULL) {
        return NULL;
    }
    region->start = PROCESS_IPC_BUFFER;
    // TODO: What's the size of the IPC buffer?
    region->end = PROCESS_VMEM_START - 1;
    region->perms = seL4_AllRights;
    region->next = NULL;
    return region;
}

/**
 * Create statically positioned regions
 * @return error code or 0 for success
 */
static int init_regions(sos_proc_t *proc) {
    sos_region_t* ipc_buf = init_ipc_buf_region();
    sos_region_t* stack = init_stack_region();
    if (stack == NULL || ipc_buf == NULL) {
        return ENOMEM;
    }
    ipc_buf->next = stack;
    proc->vspace.regions = ipc_buf;
    printf("Default regions initialised\n");
    return 0;
}

static void init_cspace(sos_proc_t *proc) {
    /* Create a simple 1 level CSpace */
    proc->cspace = cspace_create(1);
    assert(proc->cspace != NULL);
    printf("CSpace initialised\n");
}

static void init_vspace(sos_proc_t *proc) {
    int err;
    proc->vspace.pd_addr = ut_alloc(seL4_PageDirBits);
    conditional_panic(!proc->vspace.pd_addr,
                      "No memory for new Page Directory");
    err = cspace_ut_retype_addr(proc->vspace.pd_addr,
                                seL4_ARM_PageDirectoryObject,
                                seL4_PageDirBits,
                                cur_cspace,
                                &proc->vspace.pd);
    conditional_panic(err, "Failed to allocate page directory cap for client");
    printf("VSpace initialised\n");
}

static seL4_Word _lookup_faddr(sos_proc_t *proc, seL4_Word vaddr) {
    seL4_Word pd_idx = (vaddr >> 20);
    sos_pde_t pde = proc->vspace.pde[pd_idx];

    seL4_Word pt_idx = ((vaddr & PT_MASK) >> 12);
    printf("lookup idx: for %x => %u\n", vaddr, pt_idx);
    return ((seL4_Word*)pde.pt)[pt_idx];
}

static void init_tcb(sos_proc_t *proc, seL4_CPtr user_ep_cap) {
    int err;
    printf("TCP Starting...\n");

    /* Create a new TCB object */
    proc->tcb_addr = ut_alloc(seL4_TCBBits);
    conditional_panic(!proc->tcb_addr, "No memory for new TCB");
    err =  cspace_ut_retype_addr(proc->tcb_addr,
                                 seL4_TCBObject,
                                 seL4_TCBBits,
                                 cur_cspace,
                                 &proc->tcb_cap);
    conditional_panic(err, "Failed to create TCB");

    /* Configure the TCB */
    err = seL4_TCB_Configure(proc->tcb_cap, user_ep_cap, TEST_PRIORITY,
                             proc->cspace->root_cnode, seL4_NilData,
                             proc->vspace.pd, seL4_NilData, PROCESS_IPC_BUFFER,
                             proc->ipc_buf_cap);
    conditional_panic(err, "Unable to configure new TCB");
    printf("TCB initialised\n");
}

/**
 * Initialise the endpoint
 */
static seL4_CPtr init_ep(sos_proc_t *proc, seL4_CPtr fault_ep) {
    seL4_CPtr user_ep_cap;

    /* Copy the fault endpoint to the user app to enable IPC */
    user_ep_cap = cspace_mint_cap(proc->cspace,
                                  cur_cspace,
                                  fault_ep,
                                  seL4_AllRights,
                                  seL4_CapData_Badge_new(TEST_EP_BADGE));
    /* should be the first slot in the space, hack I know */
    assert(user_ep_cap == 1);
    assert(user_ep_cap == USER_EP_CAP);
    printf("EP initialised\n");
    return user_ep_cap;
}

static void init_ipc_buf(sos_proc_t *proc) {
    int err;
    proc->ipc_buf_addr = ut_alloc(seL4_PageBits);
    conditional_panic(!proc->ipc_buf_addr, "No memory for ipc buffer");
    err =  cspace_ut_retype_addr(proc->ipc_buf_addr,
                                 seL4_ARM_SmallPageObject,
                                 seL4_PageBits,
                                 cur_cspace,
                                 &proc->ipc_buf_cap);
    conditional_panic(err, "Unable to allocate page for IPC buffer");
}

static void ipc_map_buf(sos_proc_t *proc) {
    int err;
    err = map_page(proc->ipc_buf_cap, proc->vspace.pd, PROCESS_IPC_BUFFER, seL4_AllRights,
                   seL4_ARM_Default_VMAttributes);
    if (err) {
        ERR("Failed to map ipc_map_buf: %d\n", err);
    }
}

/**
 * Create a new process
 * @return error code or 0 for success
 */
int process_create(seL4_CPtr fault_ep) {
    int err;
    seL4_CPtr user_ep_cap;
    err = init_regions(curproc);
    if (err) {
        WARN("CREATING REGIONS FAILED\n");
        return err;
    }
    init_vspace(curproc);
    init_cspace(curproc);
    init_ipc_buf(curproc);
    user_ep_cap = init_ep(curproc, fault_ep);
    init_tcb(curproc, user_ep_cap);
    process_map_page(curproc, PROCESS_STACK_TOP - (1 << seL4_PageBits));
    ipc_map_buf(curproc);
    printf("Process created\n");
    return 0;
}
