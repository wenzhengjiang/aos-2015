/** process.c --- Process management **/

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

#define verbose 5
#include <log/debug.h>
#include <log/panic.h>

/* This is the index where a clients syscall enpoint will
 * be stored in the clients cspace. */
#define USER_EP_CAP         (1)
#define TEST_PRIORITY       (0)

static sos_proc_t* proc_table[MAX_PROCESS_NUM] ;

static pid_entry_t * free_proc_head = NULL ,*free_proc_tail = NULL, *running_proc_head = NULL;
static pid_entry_t pid_table[MAX_PROCESS_NUM];

static sos_proc_t *curproc = NULL;
extern jmp_buf ipc_event_env;
extern size_t process_frames;
static size_t running_processes = 0;
extern size_t addrspace_pages;

static pid_entry_t *last_evicted_proc = NULL;

static void init_cspace(sos_proc_t *proc) {
    /* Create a simple 1 level CSpace */
    proc->cspace = cspace_create(1);
    assert(proc->cspace != NULL);
}

static size_t page_threshold() {
    return (addrspace_pages / running_processes) + 1;
}

sos_proc_t *select_eviction_process(void) {

    if (last_evicted_proc == NULL) last_evicted_proc = running_proc_head;

    pid_entry_t *cur_proc= last_evicted_proc->next;
    int cnt = 0;
    assert(running_proc_head);
    assert(last_evicted_proc);
    assert(proc_table[last_evicted_proc->pid]);
    while(last_evicted_proc != cur_proc) {
        if (++cnt > running_processes) break;
        if (cur_proc == NULL) cur_proc = running_proc_head;
        assert(cur_proc->running);

        int i = cur_proc->pid;
        // Random starvation intervention
        if (rand() % 20 == 0) {
            last_evicted_proc = &pid_table[i];
            return proc_table[i];
        }
        if (cur_proc->pid == current_process()->pid) {
            cur_proc = cur_proc->next;
            continue;
        }

        printf("page_threshold: %u, %d, %d\n", page_threshold(), i, cur_proc->running);
        assert(proc_table[i]);
        assert(proc_table[i]->vspace);
        printf("mapped: %u\n", proc_table[i]->vspace->pages_mapped);
        if (proc_table[i]->vspace->pages_mapped > page_threshold()) {
            last_evicted_proc = &pid_table[i];
                assert(proc_table[last_evicted_proc->pid]);
            return proc_table[i];
        }
        cur_proc = cur_proc->next;
    }
    last_evicted_proc = &pid_table[current_process()->pid];
    assert(proc_table[last_evicted_proc->pid]);
    return current_process();
}

static void init_tcb(sos_proc_t *proc) {
    int err;

    /* Create a new TCB object */
    if(!proc->tcb_addr) {
        proc->tcb_addr = ut_alloc(seL4_TCBBits);
        conditional_panic(!proc->tcb_addr, "No memory for new TCB");
    }
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

static int get_next_pid() {
    if (free_proc_head == NULL) return -1;
    pid_entry_t * pe = free_proc_head;
    assert(free_proc_head != free_proc_head->next);
    free_proc_head = free_proc_head->next;
    if (free_proc_head) free_proc_head->prev = NULL;
    if (!free_proc_head) free_proc_tail = NULL;
    if (pe) assert(!pe->running);
    return pe->pid;
}
static void proc_table_init(void) {
    for (int i = 1; i < MAX_PROCESS_NUM; i++) {
        pid_table[i].pid = i;
        pid_table[i].running = false;
        if (i == MAX_PROCESS_NUM-1) pid_table[i].next = NULL; 
        else pid_table[i].next = &pid_table[i+1];
        if (i == 1) pid_table[i].prev = NULL;
        else pid_table[i].prev = &pid_table[i-1];
    }
    free_proc_head = &pid_table[1];
    running_proc_head = NULL;
}
/**
 * Create a new process
 * @return error code or 0 for success
 */
sos_proc_t* process_create(char *name, seL4_CPtr fault_ep) {
    //dprintf(3, "process_create\n");
    dprintf(3, "process_create\n");
    sos_proc_t* proc;
    if (!current_process()) {
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
        if(proc->pid < 1) return NULL;
        proc->start_time = time_stamp();
        proc_table[proc->pid] = proc;
        current_process()->cont.spawning_process = proc;
        proc->cont.parent_pid = current_process()->pid;
    } else {
        proc = effective_process();
    }
    if (!proc->vspace || !proc->vspace->sos_ipc_buf_addr) {
        assert(proc->pid >= 1);
        as_create(&proc->vspace);
        init_cspace(proc);
        assert(proc->vspace);
        proc->user_ep_cap = init_ep(proc, fault_ep);
        printf("Initialising TCB\n");
        init_tcb(proc);
        printf("Finished initialising TCB\n");
    }
    if (!proc->fd_table) {
        int err = init_fd_table(proc);
        proc->status.pid = proc->pid;
        proc->status.size = proc->frames_available * PAGE_SIZE;
        proc->status.stime = time_stamp() / 1000;
        strncpy(proc->status.command, name, N_NAME); 
        assert(!err);
        proc_table[proc->pid] = proc;
        printf("Process create finished\n");
        //dprintf(3, "process_create finished\n");
        assert(proc->vspace);
    }
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
    printf("process delete\n");
    assert(proc);

    {
        pid_entry_t* p = &pid_table[proc->pid];
        pid_entry_t* prev = p->prev, *next = p->next;
        assert(prev != p && next != p);

        p->prev= free_proc_tail;
        p->next= NULL;
        free_proc_tail = p;
        if (!free_proc_head) free_proc_head = p;
        if (p->prev) p->prev->next= p;

        if(p->running) {
            p->running = false;

            if (prev) {
                prev->next = next;
                assert(prev->running);
            }
            else {
                if (next != NULL) {
                    running_proc_head = next;
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
    //process_deregister_wait(proc, proc->pid);
    dprintf(4, "[AS] fd_table\n");
    free_fd_table(proc->fd_table);
    iov_free(proc->cont.iov);
    process_wake_waiters(proc);
    process_free_pid_queue(proc);
    if (proc->cspace) {
        dprintf(4, "[AS] destroying cspace\n");
        cspace_destroy(proc->cspace);
    }
    proc_table[proc->pid] = NULL;

    if(proc->frames_available != 0) {
        dprintf(1, "%d frames remaining\n", proc->frames_available);
    }
    free(proc);
    running_processes--;
    //if (running_processes == 0) {
    //    longjmp(ipc_event_env, SYSCALL_INIT_PROC_TERMINATED);
    //}
    printf("process delete\n");
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

// TODO: Remove this function.  maybe?
void process_create_page(seL4_Word vaddr, seL4_CapRights rights) {
    sos_addrspace_t* as = effective_process()->vspace;
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
    pid_entry_t* pid_queue = proc->pid_queue;
    for (pid_entry_t* p = pid_queue; p; p = p->next) {
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
    printf("start_process\n");
    int err;

    /* These required for setting up the TCB */
    seL4_UserContext context;

    /* These required for loading program sections */
    printf("check continuation\n");
    proc = process_create(app_name, fault_ep);

    if (!proc) return -1;

    if (!proc->cont.binary_nfs_open) {
        /* TODO: Fix magic numbers */
        proc->cont.fd = BINARY_READ_FD;
        proc->cont.file_mode = FM_READ;
        strncpy(proc->cont.path, app_name, MAX_FILE_PATH_LENGTH);
        proc->cont.binary_nfs_open = true;
        assert(proc->fd_table);
        assert(proc->fd_table[proc->cont.fd]);
        assert(proc->fd_table[proc->cont.fd]->io);
        assert(effective_process());
        (proc->fd_table[proc->cont.fd])->io->open(proc->cont.path, proc->cont.file_mode);
        longjmp(ipc_event_env, -1);
    }

    if (!proc->cont.binary_nfs_read) {
        frame_alloc(&proc->cont.elf_load_addr);
        assert(proc->cont.elf_load_addr);
        proc->cont.iov = iov_create(proc->cont.elf_load_addr, PAGE_SIZE, NULL, NULL, true);
        proc->cont.binary_nfs_read = true;
        (proc->fd_table[proc->cont.fd])->io->read(proc->cont.iov, proc->cont.fd, PAGE_SIZE);
        longjmp(ipc_event_env, -1);
        dprintf(1, "\nStarting \"%s\"...\n", app_name);
    }

    sos_addrspace_t *as = proc_as(proc);
    assert(as);

    if (!proc->cont.as_activated) {
        /* load the elf image */
        err = elf_load(proc, (char*)proc->cont.elf_load_addr);
        if (err) {
            assert(effective_process() != current_process());
            sos_unmap_frame(proc->cont.elf_load_addr);
            process_delete(effective_process());
            return -1;
        }
        proc->cont.as_activated = true;
    }
    as_activate(as);

    {
    pid_entry_t * pe = &pid_table[proc->pid];
    pe->next = running_proc_head;
    pe->prev = NULL;
    running_proc_head = pe;
    if (pe->next) pe->next->prev = pe, assert(pe->next->running);
    pe->running = true;
    running_processes++;
    }
    /* Start the new process */
    memset(&context, 0, sizeof(context));
    context.pc = elf_getEntryPoint((void*)proc->cont.elf_load_addr);
    context.sp = PROCESS_STACK_TOP;
    assert(proc && proc->tcb_cap);
    seL4_TCB_WriteRegisters(proc->tcb_cap, 1, 0, 2, &context);
    sos_unmap_frame(proc->cont.elf_load_addr);
    memset(&proc->cont, 0, sizeof(cont_t));

    return proc->pid;
}


int register_to_proc(sos_proc_t* proc, pid_t pid) {
    assert(proc);
    pid_entry_t * pe = malloc(sizeof(pid_entry_t));
    if (pe == NULL) return ENOMEM;
    pe->pid = pid;
    pe->next = proc->pid_queue;
    if (pe->next) pe->next->prev = pe;
    pe->prev = NULL;
    proc->pid_queue = pe;
    return 0;
}

int register_to_all_proc(pid_t pid) {
    int err = 0;
    for (int i = 1; i < MAX_PROCESS_NUM; i++) {
        if(proc_table[i] != NULL && pid != i) {
            if((err = register_to_proc(proc_table[i], pid))) {
                for (int j = 0; j < i; j++) if (proc_table[i] != NULL && pid != i) {
                    //TODO unregister 
                }
                return err;
            }
        }
    }
    return err;
}
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
