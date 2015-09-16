/** process.c --- Process management **/

#include <sel4/sel4.h>
#include <stdlib.h>
#include <limits.h>
#include <device/vmem_layout.h>
#include <errno.h>
#include <assert.h>
#include <ut/ut.h>
#include <device/mapping.h>
#include <string.h>
#include <nfs/nfs.h>

#include "process.h"
#include "addrspace.h"
#include "page_replacement.h"
#include "frametable.h"
#include "serial.h"

#define verbose 0
#include <log/debug.h>
#include <log/panic.h>

/* This is the index where a clients syscall enpoint will
 * be stored in the clients cspace. */
#define USER_EP_CAP         (1)
#define TEST_PRIORITY       (0)
#define TEST_EP_BADGE       (101)
#define FD_TABLE_SIZE       (1024)

sos_proc_t test_proc;
sos_proc_t *curproc = &test_proc;

static void init_cspace(sos_proc_t *proc) {
    /* Create a simple 1 level CSpace */
    proc->cspace = cspace_create(1);
    assert(proc->cspace != NULL);
}

static void init_tcb(sos_proc_t *proc) {
    int err;

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
    return user_ep_cap;
}

/**
 * Create a new process
 * @return error code or 0 for success
 */
int process_create(seL4_CPtr fault_ep) {
    memset((void*)curproc, 0, sizeof(sos_proc_t));
    curproc->pid = 1;
    curproc->vspace = as_create();
    init_cspace(curproc);
    curproc->user_ep_cap = init_ep(curproc, fault_ep);
    init_tcb(curproc);
    init_fd_table();
    return 0;
}

sos_addrspace_t *current_as(void) {
    return proc_as(curproc);
}

sos_proc_t *current_process(void) {
    return curproc;
}

void process_create_page(seL4_Word vaddr, seL4_CapRights rights) {
    sos_addrspace_t* as = current_process()->vspace;
    sos_proc_t* proc = current_process();
    if (!frame_available_frames()) {
        swap_evict_page(as);
    }
    printf("start to create page\n");
    if (proc->cont.page_replacement_victim) {
        printf("sos_unmap_frame, %x\n", proc->cont.page_replacement_victim->addr);
        sos_unmap_frame(proc->cont.page_replacement_victim->addr);
        if (proc->cont.page_replacement_victim->swaddr == (unsigned)-1) {
            assert(!"Victim has no swap address!");
        }
    }
   // printf("as_create_page\n");
    as_create_page(as, vaddr, rights);
}

of_entry_t *fd_lookup(sos_proc_t *proc, int fd) {
    assert(proc);
    return proc->fd_table[fd];
}

sos_proc_t *process_lookup(pid_t pid) {
    (void)pid;
    // TODO: Implement me
    return &test_proc;
}

int fd_free(sos_proc_t* proc, int fd) {
    assert(proc);
    assert(proc->fd_table[fd]);
    if (proc->fd_table[fd] == NULL) {
        printf("fd %d not found to close\n", fd);
        return -1;
    }
    proc->fd_table[fd]->io = NULL;
    proc->fd_table[fd] = NULL;
    dprintf(3, "fd %d closed okay\n", fd);
    return 0;
}
