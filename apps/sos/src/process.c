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
#include <clock/clock.h>

#include "process.h"
#include "addrspace.h"
#include "page_replacement.h"
#include "frametable.h"
#include "serial.h"
#include <elf/elf.h>
#include "elf.h"
#include "file.h"

#define verbose 5
#include <log/debug.h>
#include <log/panic.h>

/* This is the index where a clients syscall enpoint will
 * be stored in the clients cspace. */
#define USER_EP_CAP         (1)
#define TEST_PRIORITY       (0)

static sos_proc_t* proc_table[MAX_PROCESS_NUM] ;

static sos_proc_t *curproc = NULL;
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
                                  seL4_CapData_Badge_new((unsigned)proc->pid));
    /* should be the first slot in the space, hack I know */
    assert(user_ep_cap == 1);
    assert(user_ep_cap == USER_EP_CAP);
    return user_ep_cap;
}

static int init_fd_table(sos_proc_t *proc) {
    frame_alloc((seL4_Word*)&proc->fd_table);
    if (!proc->fd_table) {
        return ENOMEM;
    }
    if(fd_create_fd(proc->fd_table, 0, &serial_io, FM_WRITE,1) < 0) return ENOMEM;
    if(fd_create_fd(proc->fd_table, 0, &serial_io, FM_WRITE,2) < 0) return ENOMEM;

    return 0;
}

static int get_next_pid() {
    static int next_pid = 1;
    int cnt = 0;
    while (next_pid == 0 || proc_table[next_pid]) {
        next_pid = (next_pid+1) % MAX_PROCESS_NUM;
        cnt++;
        if (cnt == MAX_PROCESS_NUM) return -1;
    }
    return next_pid;
}
/**
 * Create a new process
 * @return error code or 0 for success
 */
sos_proc_t* process_create(char *name, seL4_CPtr fault_ep) {
    //dprintf(3, "process_create\n");
    sos_proc_t* proc = malloc(sizeof(sos_proc_t));
    if (proc == NULL) {
        return NULL;
    }
    memset((void*)proc, 0, sizeof(sos_proc_t));
    proc->pid = get_next_pid();
    if (proc->pid == -1) {
        free(proc);
        return NULL;
    }
    proc->vspace = as_create();
    init_cspace(proc);
    assert(proc->vspace);
    proc->user_ep_cap = init_ep(proc, fault_ep);
    printf("Initialising TCB\n");
    init_tcb(proc);
    printf("Finished initialising TCB\n");
    int err = init_fd_table(proc);
    proc->status.pid = proc->pid;
    proc->status.size = 0;
    proc->status.stime = time_stamp() / 1000;
    strncpy(proc->status.command, name, N_NAME); 

    assert(!err);
    proc_table[proc->pid] = proc;
    printf("Process create finished\n");
    //dprintf(3, "process_create finished\n");
    return proc;
}

static void process_free_pid_queue(sos_proc_t *proc) {
    pid_entry_t *node;
    dprintf(3, "[AS] freeing pid_queue\n");
    for (node = proc->pid_queue; proc->pid_queue != NULL; node = node->next) {
        node = proc->pid_queue->next;
        free(proc->pid_queue);
        proc->pid_queue = node;
    }
    dprintf(4, "[AS] pid_queue free'd\n");
}

void process_delete(sos_proc_t* proc) {
    assert(proc);

    cspace_revoke_cap(cur_cspace, proc->tcb_cap);
    cspace_err_t err = cspace_delete_cap(cur_cspace, proc->tcb_cap);
    if (err != CSPACE_NOERROR) {
        ERR("[PROC]: failed to delete tcb cap\n");
    }
    ut_free(proc->tcb_addr, seL4_TCBBits);
    as_free(proc->vspace);
    process_deregister_wait(proc, proc->pid);
    free_fd_table(proc->fd_table);
    iov_free(proc->cont.iov);
    process_wake_waiters(proc);
    process_free_pid_queue(proc);
    cspace_destroy(proc->cspace);
    free(proc);
}

sos_addrspace_t *current_as(void) {
    return proc_as(curproc);
}

void set_current_process(pid_t pid) {
    assert(pid > 0 && pid < MAX_PROCESS_NUM);
    curproc = process_lookup(pid);
}

sos_proc_t *current_process(void) {
    return curproc;
}

// TODO: Remove this function.  maybe?
void process_create_page(seL4_Word vaddr, seL4_CapRights rights) {
    sos_addrspace_t* as = current_process()->vspace;
    int err = as_create_page(as, vaddr, rights);
    // TODO: Return ENOMEM to client.
    assert(!err);
}

of_entry_t *fd_lookup(sos_proc_t *proc, int fd) {
    assert(proc);
    if(!(fd >= 0 && fd <= MAX_FD))
        return NULL;
    return proc->fd_table[fd];
}

sos_proc_t *process_lookup(pid_t pid) {
    if(!(pid > 0 && pid < MAX_PROCESS_NUM)) {
        return NULL;
    }
    return proc_table[pid];
}

int process_wake_waiters(sos_proc_t *proc) {
    (void)proc;
    ERR("[PROCESS] process_wake_waiters(): implement me");

    pid_entry_t* pid_queue = proc->pid_queue;
    for (pid_entry_t* p = pid_queue; p; p = p->next) {
        sos_proc_t* wake_proc = process_lookup(p->pid);
        assert(wake_proc->waiting_pid == proc->pid || wake_proc->waiting_pid == -1);
        // only one process can get returned pid
        //if(p == pid_queue) syscall_end_continuation(wake_proc, proc->pid, true);
        //else syscall_end_continuation(proc, -1, false);
        syscall_end_continuation(proc, proc->pid, true);
    }
    return 0;
}

pid_t start_process(char* app_name, seL4_CPtr fault_ep) {
    printf("start_process\n");
    int err;

    /* These required for setting up the TCB */
    seL4_UserContext context;

    /* These required for loading program sections */
    char* elf_base;
    unsigned long elf_size;
    printf("process_create\n");
    sos_proc_t* proc = process_create(app_name, fault_ep);
    if (!proc) return -1;

    printf("proc_as\n");
    sos_addrspace_t *as = proc_as(proc);

    /* parse the cpio image */
    dprintf(1, "\nStarting \"%s\"...\n", app_name);
    elf_base = cpio_get_file(_cpio_archive, app_name, &elf_size);
    conditional_panic(!elf_base, "Unable to locate cpio header");
    /* load the elf image */
    err = elf_load(proc, proc->vspace->sos_pd_cap, elf_base);
    conditional_panic(err, "Failed to load elf image");
    as_activate(as);

    /* Start the new process */
    memset(&context, 0, sizeof(context));
    context.pc = elf_getEntryPoint(elf_base);
    context.sp = PROCESS_STACK_TOP;
    assert(proc && proc->tcb_cap);
    seL4_TCB_WriteRegisters(proc->tcb_cap, 1, 0, 2, &context);
    return proc->pid;
}


int register_to_all_proc(pid_t pid) {
    int err = 0;
    for (int i = 1; i < MAX_PROCESS_NUM; i++) {
        if(proc_table[i] != NULL) {
            if(err = register_to_proc(proc_table[i], pid))
                return err;
        }
    }
    return err;
}
int register_to_proc(sos_proc_t* proc, pid_t pid) {
    assert(proc);
    pid_entry_t * pe = malloc(sizeof(pid_entry_t));
    pe->pid = pid;
    if (pe == NULL) return ENOMEM;
    pe->next = proc->pid_queue;
    proc->pid_queue = pe;
    return 0;
}

static int deregister_to_proc(sos_proc_t* proc, pid_t pid) {
    assert(proc);
    assert(proc->pid_queue);
    if (proc->pid_queue->pid == pid) {
        proc->pid_queue = proc->pid_queue->next;
        return 0;
    }
    pid_entry_t *p = proc->pid_queue;
    bool found = false;
    while (p->next) {
        if (p->next->pid == pid) {
            found = true;
            pid_entry_t* next = p->next;
            p->next = p->next->next;
            free(next);
        }
        p = p->next;
    }
    return !found;
}

static int deregister_to_all_proc(pid_t pid) {
    int err = 0;
    for (int i = 1; i < MAX_PROCESS_NUM; i++) {
        if(proc_table[i]) {
            err = deregister_to_proc(proc_table[i], pid);
        }
    }
    return err;
}

int process_deregister_wait(sos_proc_t* proc, pid_t pid) {
    if (pid == 0) {
        return 0;
    } else if (pid == -1) {
        return deregister_to_all_proc(pid);
    }
    return deregister_to_proc(proc, pid);
}

int get_all_proc_stat(char *buf, size_t size) {
    assert(buf);
    int offset = 0;
    for (int i = 1;i < MAX_PROCESS_NUM && offset < size; i++) {
        sos_proc_t * proc = proc_table[i];
        if (proc) {
            memcpy(buf+offset, (char*)&proc->status, sizeof(sos_process_t));
            offset += sizeof(sos_process_t);
        }
    }
    return offset;
}
