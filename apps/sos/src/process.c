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
#include "addrspace.h"
#include "frametable.h"

#define verbose 5
#include <log/debug.h>
#include <log/panic.h>

/* This is the index where a clients syscall enpoint will
 * be stored in the clients cspace. */
#define USER_EP_CAP          (1)
#define TEST_PRIORITY         (0)
#define TEST_EP_BADGE         (101)

sos_proc_t test_proc;
sos_proc_t *curproc = &test_proc;

static void init_cspace(sos_proc_t *proc) {
    /* Create a simple 1 level CSpace */
    proc->cspace = cspace_create(1);
    assert(proc->cspace != NULL);
    printf("CSpace initialised\n");
}

static void init_tcb(sos_proc_t *proc) {
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
    seL4_CPtr ipc_cap = frame_cap(proc->vspace->sos_ipc_buf_addr);
    err = seL4_TCB_Configure(proc->tcb_cap, proc->user_ep_cap, TEST_PRIORITY,
                             proc->cspace->root_cnode, seL4_NilData,
                             proc->vspace->sos_pd_cap, seL4_NilData,
                             PROCESS_IPC_BUFFER, ipc_cap);
    conditional_panic(err, "Unable to configure new TCB");
    printf("TCB initialised\n");
}

sos_addrspace_t *proc_as(sos_proc_t *proc) {
    return proc->vspace;
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

/**
 * Create a new process
 * @return error code or 0 for success
 */
int process_create(seL4_CPtr fault_ep) {
    curproc->vspace = as_create();
    init_cspace(curproc);
    curproc->user_ep_cap = init_ep(curproc, fault_ep);
    init_tcb(curproc);
    printf("Process created\n");
    return 0;
}

sos_proc_t *current_process(void) {
    return curproc;
}
