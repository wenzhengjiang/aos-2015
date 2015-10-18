/**
 * @file process.c
 * @brief Implementation of process interface
 */

#include <sel4/sel4.h>
#include <setjmp.h>
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
#include <syscallno.h>
#include "process.h"
#include "addrspace.h"
#include "page_replacement.h"
#include "frametable.h"
#include "serial.h"
#include <elf/elf.h>
#include "elf.h"
#include "file.h"
#include "sos_nfs.h"

#define verbose 0
#include <log/debug.h>
#include <log/panic.h>

/* This is the index where a clients syscall enpoint will
 * be stored in the clients cspace. */
#define USER_EP_CAP         (1)
#define TEST_PRIORITY       (0)

/* process table */
static sos_proc_t* proc_table[MAX_PROCESS_NUM] ;

/* pid queue of all unused pids (double linked list) */
static pid_entry_t * free_pid_head = NULL ,*free_pid_tail = NULL;
/* pid queue of all running process (single linked list) */
static pid_entry_t *running_pid_head = NULL;
/* pid table */
static pid_entry_t pid_table[MAX_PROCESS_NUM];

/* current client sos is serving */
static sos_proc_t *curproc = NULL;
extern jmp_buf ipc_event_env;
/* number of running processes */
static size_t running_pidesses = 0;
/* number of swapable pages in memory */
extern size_t addrspace_pages;
/* last process evicted to swap out page */
static pid_entry_t *last_evicted_proc = NULL;

/**
 * @brief minimum memory pages a process should have to be evicted to swap out pages
 */
static size_t page_threshold() {
    return (addrspace_pages / running_pidesses) + 1;
}

static int init_tcb(sos_proc_t *proc) {
    int err;

    /* Create a new TCB object */
    if(!proc->tcb_addr) {
        proc->tcb_addr = ut_alloc(seL4_TCBBits);
        if (!proc->tcb_addr) {
            ERR("No memory for new TCB");
            return ENOMEM;
        }
    }
    err =  cspace_ut_retype_addr(proc->tcb_addr,
            seL4_TCBObject,
            seL4_TCBBits,
            cur_cspace,
            &proc->tcb_cap);
    if (err) {
        ERR("Failed to create TCB");
        return err;
    }

    /* Configure the TCB */
    seL4_CPtr ipc_cap = frame_cap(proc->vspace->sos_ipc_buf_addr);
    err = seL4_TCB_Configure(proc->tcb_cap, proc->user_ep_cap, TEST_PRIORITY,
            proc->cspace->root_cnode, seL4_NilData,
            proc->vspace->sos_pd_cap, seL4_NilData,
            PROCESS_IPC_BUFFER, ipc_cap);
    if (err) {
        ERR("Unable to configure new TCB");
        return err;
    }
    return 0;
}

sos_addrspace_t *proc_as(sos_proc_t *proc) {
    return proc->vspace;
}

/**
 * Initialize the endpoint
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

/**
 * @brief allocate fd table and create STDOUT, STDERR, BRINARY_READ_FD
 */
static int init_fd_table(sos_proc_t *proc) {
    if (proc->fd_table) return 0;
    frame_alloc((seL4_Word*)&proc->fd_table);
    if (!proc->fd_table) {
        return ENOMEM;
    }
    if(fd_create_fd(proc->fd_table, NULL, &serial_io, FM_WRITE, 1) < 0) return ENOMEM;
    if(fd_create_fd(proc->fd_table, NULL, &serial_io, FM_WRITE, 2) < 0) return ENOMEM;
    if(fd_create_fd(proc->fd_table, NULL, &nfs_io, FM_READ, BINARY_READ_FD) < 0) return ENOMEM;

    return 0;
}

/**
 * @brief Get next pid in free pid. O(1) time
 *
 */
static int get_next_pid() {
    if (free_pid_head == NULL) return -1;
    pid_entry_t * pe = free_pid_head;
    assert(free_pid_head != free_pid_head->next);
    free_pid_head = free_pid_head->next;
    if (free_pid_head) free_pid_head->prev = NULL;
    if (!free_pid_head) free_pid_tail = NULL;
    if (pe) assert(!pe->running);
    return pe->pid;
}

/**
 * @brief Initialize free pid queue and running pid queue
 */
static void proc_table_init(void) {
    for (int i = 1; i < MAX_PROCESS_NUM; i++) {
        pid_table[i].pid = i;
        pid_table[i].running = false;
        if (i == MAX_PROCESS_NUM-1) pid_table[i].next = NULL; 
        else pid_table[i].next = &pid_table[i+1];
        if (i == 1) pid_table[i].prev = NULL;
        else pid_table[i].prev = &pid_table[i-1];
    }
    free_pid_head = &pid_table[1];
    running_pid_head = NULL;
}

static void process_free_waiter_queue(sos_proc_t *proc) {
    pid_entry_t *node;
    dprintf(3, "[AS] freeing waiter_queue\n");
    for (node = proc->waiter_queue; proc->waiter_queue != NULL; node = node->next) {
        node = proc->waiter_queue->next;
        free(proc->waiter_queue);
        proc->waiter_queue = node;
    }
    dprintf(4, "[AS] waiter_queue free'd\n");
}

/**
 * @brief remove a process to a process's waiter list
 *
 * @param pid
 * @param waiter
 */
static void unregister_to_proc(pid_t pid, pid_t waiter) {
    sos_proc_t *proc = process_lookup(pid);
    assert(proc);
    pid_entry_t* p = proc->waiter_queue;
    while (p) {
        if (p->pid == waiter) p->pid = 0;
        p = p->next;
    }
}

static void unregister_to_all(pid_t pid) {
    pid_entry_t * p = running_pid_head;
    while (p) {
        unregister_to_proc(p->pid, pid);
        p = p->next;
    }
}
/**
 * @brief unregister a process from the waiters list of process it's waiting
 *
 */
static int process_unregister_wait(sos_proc_t *proc) {
    assert(proc); 
    if (proc->waiting_pid == 0) return 0;
    else if (proc->waiting_pid == -1) {
        unregister_to_all(proc->pid);
    } else {
        sos_proc_t * w_proc  = process_lookup(proc->waiting_pid);
        if (!w_proc) return 0; 
        unregister_to_proc(w_proc->pid, proc->pid);
    }
    return 0;
}

/**
 * @brief  Select a running process and swap out one of its page to disk
 *         The algorithm is basically start from last evicted process to 
 *         choose a process which has more page in memory than page_threshold().
 *         If it can't find one, it returns concurrent process.
 */
sos_proc_t *select_eviction_process(void) {

    if (last_evicted_proc == NULL) last_evicted_proc = running_pid_head;

    pid_entry_t *proc= last_evicted_proc->next;
    int cnt = 0;
    assert(running_pid_head);
    assert(last_evicted_proc);
    assert(proc_table[last_evicted_proc->pid]);
    while(last_evicted_proc != proc) {
        if (++cnt > running_pidesses) break;
        if (proc == NULL) proc = running_pid_head;
        assert(proc->running);

        int i = proc->pid;
        // Random starvation intervention
        if (rand() % 20 == 0) {
            last_evicted_proc = &pid_table[i];
            return proc_table[i];
        }
        if (proc->pid == current_process()->pid) {
            proc = proc->next;
            continue;
        }

        dprintf(3, "page_threshold: %u, %d, %d\n", page_threshold(), i, proc->running);
        assert(proc_table[i]);
        assert(proc_table[i]->vspace);
        dprintf(3, "mapped: %u, threshold = %u\n", proc_table[i]->vspace->pages_mapped, page_threshold());
        if (proc_table[i]->vspace->pages_mapped > page_threshold()) {
            dprintf(3, "evict process %d\n", i);
            last_evicted_proc = &pid_table[i];
            assert(proc_table[last_evicted_proc->pid]);
            return proc_table[i];
        }
        proc = proc->next;
    }
    last_evicted_proc = &pid_table[current_process()->pid];
    assert(proc_table[last_evicted_proc->pid]);
    return current_process();
}


/**
 * @brief   Allocate a new process and setup everything
 */
sos_proc_t* process_create(char *name, seL4_CPtr fault_ep) {
    dprintf(3, "process_create\n");
    sos_proc_t* proc;
    if (!current_process()) { // we're creating the first process
        proc_table_init();
        proc = malloc(sizeof(sos_proc_t));
        if (!proc) {
            return NULL;
        }
        memset((void*)proc, 0, sizeof(sos_proc_t));
        proc->pid = get_next_pid();
        
        proc->start_time = time_stamp();
        proc_table[proc->pid] = proc;
        proc->cont.spawning_process = (void*)-1;
        set_current_process(proc->pid);
    } else if (!current_process()->cont.spawning_process) {
        proc = malloc(sizeof(sos_proc_t));
        if (!proc) {
            return NULL;
        }
        memset((void*)proc, 0, sizeof(sos_proc_t));
        proc->pid = get_next_pid();
        if(proc->pid < 1) {
            free(proc);
            return NULL;
        }
        proc->start_time = time_stamp();
        proc_table[proc->pid] = proc;
        current_process()->cont.spawning_process = proc;
    } else {
        proc = effective_process();
    }
    if (!proc->vspace || !proc->vspace->sos_ipc_buf_addr) {
        assert(proc->pid >= 1);
        if (as_create(&proc->vspace)) {
            process_delete(proc);
            return NULL;
        }
        if ((proc->cspace = cspace_create(1)) == 0){
            process_delete(proc);
            return NULL;
        }
        proc->user_ep_cap = init_ep(proc, fault_ep);
        dprintf(3, "Initializing TCB\n");
        if (init_tcb(proc)) {
            process_delete(proc);
            return NULL;
        }
        dprintf(3, "Finished initializing TCB\n");
    }
    if (!proc->fd_table) {
        int err = init_fd_table(proc);
        if (err) {
            process_delete(proc);
            return NULL;
        }
        proc->status.pid = proc->pid;
        proc->status.size = proc->frames_available * PAGE_SIZE;
        proc->status.stime = time_stamp() / 1000;
        strncpy(proc->status.command, name, N_NAME); 
        proc_table[proc->pid] = proc;
        dprintf(3, "Process create finished\n");
    }
    return proc;
}


/**
 * @brief Free a process and all its resources, clear up everything ...
 */
void process_delete(sos_proc_t* proc) {
    dprintf(3, "process delete\n");
    assert(proc);
    // Remove it from running pid queue and add it to free pd queue
    {
        pid_entry_t* p = &pid_table[proc->pid];
        pid_entry_t* prev = p->prev, *next = p->next;
        assert(prev != p && next != p);

        p->prev= free_pid_tail;
        p->next= NULL;
        free_pid_tail = p;
        if (!free_pid_head) free_pid_head = p;
        if (p->prev) p->prev->next= p;

        if(p->running) {
            p->running = false;

            if (prev) {
                prev->next = next;
                assert(prev->running);
            }
            else {
                if (next != NULL) {
                    running_pid_head = next;
                }
            }

            if (next) {
                next->prev = prev;
                assert(next->running);
            }
            if (p == last_evicted_proc) {
                last_evicted_proc = next;
            }
        }
    }
    if(proc->tcb_cap) {
        cspace_revoke_cap(cur_cspace, proc->tcb_cap);
        cspace_err_t err = cspace_delete_cap(cur_cspace, proc->tcb_cap);

        if (err != CSPACE_NOERROR) {
            ERR("[PROC]: failed to delete tcb cap\n");
        }
    }
    if (proc->tcb_addr)
        ut_free(proc->tcb_addr, seL4_TCBBits);
    if (proc->vspace) {
        dprintf(4, "[AS] freeing vspace\n");
        as_free(proc->vspace);
    }
    dprintf(4, "[AS] degregister_wait\n");
    process_unregister_wait(proc);
    dprintf(4, "[AS] fd_table\n");
    free_fd_table(proc->fd_table);
    iov_free(proc->cont.iov);
    dprintf(4, "[AS] wake up waiters \n");
    process_wake_waiters(proc);
    process_free_waiter_queue(proc);
    if (proc->cspace) {
        dprintf(4, "[AS] destroying cspace\n");
        cspace_destroy(proc->cspace);
    }
    proc_table[proc->pid] = NULL;

    if(proc->frames_available != 0) {
        dprintf(1, "%d frames remaining\n", proc->frames_available);
    }
    free(proc);
    running_pidesses--;
    dprintf(4, "process_delete finished\n");
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

sos_proc_t *effective_process(void) {
    if (!curproc) {
        return NULL;
    }
    if (curproc->cont.spawning_process != 0 &&
        (unsigned)curproc->cont.spawning_process != (unsigned)-1) {
        return curproc->cont.spawning_process;
    }
    return curproc;
}

int process_create_page(seL4_Word vaddr, seL4_CapRights rights) {
    sos_addrspace_t* as = effective_process()->vspace;
    return as_create_page(as, vaddr, rights);
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



/**
 * @brief Wake up all waiters waiting for this process
 *
 */
int process_wake_waiters(sos_proc_t *proc) {
    (void)proc;
    pid_entry_t* waiter_queue = proc->waiter_queue;
    for (pid_entry_t* p = waiter_queue; p; p = p->next) {
        if (p->pid == 0) continue;
        sos_proc_t* wake_proc = process_lookup(p->pid);
        assert(wake_proc->waiting_pid == proc->pid || wake_proc->waiting_pid == -1);
        wake_proc->waiting_pid = 0;
        dprintf(3, "wake %d\n", p->pid);
        syscall_end_continuation(wake_proc, proc->pid, true);
    }
    return 0;
}

pid_t start_process(char* app_name, seL4_CPtr fault_ep) {
    static sos_proc_t* proc = NULL;
    dprintf(3, "start_process\n");
    int err;

    /* These required for setting up the TCB */
    seL4_UserContext context;

    /* These required for loading program sections */
    proc = process_create(app_name, fault_ep);
    sos_proc_t* cur_proc = current_process();

    if (!proc) return -1;

    if (!cur_proc->cont.binary_nfs_open) {
        cur_proc->cont.fd = BINARY_READ_FD;
        cur_proc->cont.file_mode = FM_READ;
        strncpy(cur_proc->cont.path, app_name, MAX_FILE_PATH_LENGTH);
        cur_proc->cont.binary_nfs_open = true;
        assert(proc->fd_table);
        assert(proc->fd_table[cur_proc->cont.fd]);
        assert(proc->fd_table[cur_proc->cont.fd]->io);
        assert(effective_process());
        (proc->fd_table[cur_proc->cont.fd])->io->open(cur_proc->cont.path, cur_proc->cont.file_mode);
        longjmp(ipc_event_env, -1);
    }

    if (!cur_proc->cont.binary_nfs_read) {
        frame_alloc(&cur_proc->cont.elf_load_addr);
        assert(cur_proc->cont.elf_load_addr);
        cur_proc->cont.iov = iov_create(cur_proc->cont.elf_load_addr, PAGE_SIZE, NULL, NULL, true);
        cur_proc->cont.binary_nfs_read = true;
        (proc->fd_table[cur_proc->cont.fd])->io->read(cur_proc->cont.iov, cur_proc->cont.fd, PAGE_SIZE);
        longjmp(ipc_event_env, -1);
        dprintf(1, "\nStarting \"%s\"...\n", app_name);
    }

    sos_addrspace_t *as = proc_as(proc);
    assert(as);

    if (!cur_proc->cont.as_activated) {
        /* load the elf image */
        err = elf_load(proc, (char*)cur_proc->cont.elf_load_addr);
        if (err) {
            assert(effective_process() != current_process());
            sos_unmap_frame(cur_proc->cont.elf_load_addr);
            process_delete(effective_process());
            return -1;
        }
        cur_proc->cont.as_activated = true;
    }
    as_activate(as);

    {
    pid_entry_t * pe = &pid_table[proc->pid];
    pe->next = running_pid_head;
    pe->prev = NULL;
    running_pid_head = pe;
    if (pe->next) pe->next->prev = pe, assert(pe->next->running);
    pe->running = true;
    running_pidesses++;
    }
    /* Start the new process */
    memset(&context, 0, sizeof(context));
    context.pc = elf_getEntryPoint((void*)cur_proc->cont.elf_load_addr);
    context.sp = PROCESS_STACK_TOP;
    assert(proc && proc->tcb_cap);
    seL4_TCB_WriteRegisters(proc->tcb_cap, 1, 0, 2, &context);
    sos_unmap_frame(cur_proc->cont.elf_load_addr);

    return proc->pid;
}


/**
 * @brief register proc to a process's waiter list
 *
 * @param proc waiter
 * @param pid process be waited
 *
 */
int register_to_proc(sos_proc_t* proc, pid_t pid) {
    assert(proc);
    pid_entry_t * pe = malloc(sizeof(pid_entry_t));
    if (pe == NULL) return ENOMEM;
    pe->pid = pid;
    pe->next = proc->waiter_queue;
    if (pe->next) pe->next->prev = pe;
    pe->prev = NULL;
    proc->waiter_queue = pe;
    return 0;
}

/**
 * @brief register a process to all process's waiting queue
 *
 * @return 
 */
int register_to_all_proc(pid_t pid) {
    int err = 0;
    for (int i = 1; i < MAX_PROCESS_NUM; i++) {
        if(proc_table[i] != NULL && pid != i) {
            register_to_proc(proc_table[i], pid);
        }
    }
    return err;
}

/**
 * @brief store process status into buf
 *
 * @return size of used buffer
 */
int get_all_proc_stat(char *buf, size_t maxn) {
    assert(buf);
    int offset = 0;
    int cnt = 0;
    for (int i = 1;i < MAX_PROCESS_NUM && cnt < maxn; i++) {
        sos_proc_t * proc = proc_table[i];
        if (proc) {
            memcpy(buf+offset, (char*)&proc->status, sizeof(sos_process_t));
            offset += sizeof(sos_process_t);
            cnt++;
        }
    }
    return offset;
}
