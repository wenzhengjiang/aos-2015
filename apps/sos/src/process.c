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
#include <cpio/cpio.h>

#include "process.h"
#include "addrspace.h"
#include "page_replacement.h"
#include "frametable.h"
#include "serial.h"
#include <elf/elf.h>
#include "elf.h"

#define verbose 0
#include <log/debug.h>
#include <log/panic.h>

/* This is the index where a clients syscall enpoint will
 * be stored in the clients cspace. */
#define USER_EP_CAP         (1)
#define TEST_PRIORITY       (0)
#define TEST_EP_BADGE       (101)
#define FD_TABLE_SIZE       (1024)

static sos_proc_t* proc_table[MAX_PROCESS_NUM] ;

sos_proc_t test_proc;
sos_proc_t *curproc = &test_proc;
extern char _cpio_archive[];

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

static int get_next_pid() {
    static int next_pid = 1;
    int cnt = 0;
    while (proc_table[next_pid]) {
        next_pid = (next_pid+1) % MAX_PROCESS_NUM;
        cnt++;
        if (cnt == MAX_PROCESS_NUM) return -1;
        if (next_pid == 0) next_pid++;
    }
    return next_pid;
}
/**
 * Create a new process
 * @return error code or 0 for success
 */
int process_create(seL4_CPtr fault_ep) {
    sos_proc_t* proc = malloc(sizeof(sos_proc_t));
    if (proc) {
        return ENOMEM;
    }
    memset((void*)proc, 0, sizeof(sos_proc_t));
    proc->pid = get_next_pid();
    if (proc->pid == -1)
        return ENOMEM;
    proc->vspace = as_create();
    init_cspace(proc);
    proc->user_ep_cap = init_ep(proc, fault_ep);
    init_tcb(proc);
    init_fd_table(&proc->fd_table);
    proc_table[proc->pid] = proc; 

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
    int err = as_create_page(as, vaddr, rights);
    // TODO: Return ENOMEM to client.
    assert(!err);
}

of_entry_t *fd_lookup(sos_proc_t *proc, int fd) {
    assert(proc);
    return proc->fd_table[fd];
}

sos_proc_t *process_lookup(pid_t pid) {
    if(!(pid > 0 && pid < MAX_PROCESS_NUM))
        return NULL;
    return proc_table[pid];
}

pid_t start_process(char* app_name, seL4_CPtr fault_ep) {
    int err;

    /* These required for setting up the TCB */
    seL4_UserContext context;

    /* These required for loading program sections */
    char* elf_base;
    unsigned long elf_size;
    pid_t pid = process_create(fault_ep);
    sos_proc_t *proc = process_lookup(pid);

    sos_addrspace_t *as = proc_as(proc);

    /* parse the cpio image */
    dprintf(1, "\nStarting \"%s\"...\n", app_name);
    elf_base = cpio_get_file(_cpio_archive, app_name, &elf_size);
    conditional_panic(!elf_base, "Unable to locate cpio header");
    /* load the elf image */
    err = elf_load(proc->vspace->sos_pd_cap, elf_base);
    conditional_panic(err, "Failed to load elf image");
    as_activate(as);

    /* Start the new process */
    memset(&context, 0, sizeof(context));
    context.pc = elf_getEntryPoint(elf_base);
    printf("pc = %08x\n", context.pc);
    context.sp = PROCESS_STACK_TOP;
    seL4_TCB_WriteRegisters(proc->tcb_cap, 1, 0, 2, &context);
    return pid;
}

