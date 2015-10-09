/** process.h --- Process management **/

#ifndef _PROCESS_H_
#define _PROCESS_H_

#include <nfs/nfs.h>
#include <cspace/cspace.h>
#include <stdbool.h>
#include <sel4/sel4.h>
#include "addrspace.h"
#include "file.h"
#include <syscallno.h>
#include <sos.h>

#define TEST_PROCESS_NAME             CONFIG_SOS_STARTUP_APP

#define MAX_PROCESS_NUM         (1024)

// TODO: This should now be backed by a dedicated sel4 frame as is big
typedef struct continuation {
    seL4_CPtr reply_cap;
    int fd;
    int counter;
    iovec_t* iov;
    int target;
    seL4_Word ipc_label;
    seL4_MessageInfo_t ipc_message;
    seL4_Word syscall_number;
    seL4_Word vm_fault_type;
    seL4_Word client_addr;
    seL4_Word elf_load_addr;
    seL4_Word page_replacement_request;
    seL4_Word original_page_addr;
    bool create_page_done;
    bool as_activated;
    uint32_t cookie;
    bool binary_nfs_open;
    bool binary_nfs_read;
    pte_t* page_replacement_victim;
    size_t reply_length;
    size_t length_arg;
    int position_arg;
    fmode_t file_mode;
    int parent_pid;
    sos_vaddr swap_page;
    size_t swap_file_offset;
    size_t swap_cnt;
    // Number of times a continuation has been started
    int syscall_loop_initiations;
    bool handler_initiated;
    bool swap_status;
    char path[MAX_FILE_PATH_LENGTH];
    pid_t pid;
    char* proc_stat_buf;
    size_t proc_stat_n;
    int elf_header;
    unsigned elf_segment_pos;
    void* spawning_process;
    void* page_eviction_process;
    seL4_Word alloc_page_frame;
} cont_t;

typedef struct pid_entry {
    pid_t pid;
    struct pid_entry* next;
} pid_entry_t;

typedef struct process {
    int pid;
    sos_addrspace_t *vspace;
    seL4_Word tcb_addr;
    seL4_TCB tcb_cap;
    cspace_t *cspace;
    seL4_CPtr user_ep_cap;
    fd_table_t fd_table;
    cont_t cont;

    pid_t waiting_pid; // pid of process I'm waiting. -1: any, 0: none
    pid_entry_t* pid_queue; // processes waiting for me

    sos_process_t status;

    int frame_cnt;
    int frame_cnt2;
} sos_proc_t;


sos_proc_t* process_create(char* name, seL4_CPtr fault_ep);
void process_delete(sos_proc_t* proc);
sos_addrspace_t *proc_as(sos_proc_t *proc);
sos_addrspace_t *current_as(void);
sos_proc_t *current_process(void);
sos_proc_t *effective_process(void);
sos_proc_t *process_lookup(pid_t pid);
of_entry_t *fd_lookup(sos_proc_t *proc, int fd);
void process_create_page(seL4_Word vaddr, seL4_CapRights rights);
pid_t start_process(char* app_name, seL4_CPtr fault_ep) ;
void set_current_process(pid_t pid);
int get_all_proc_stat(char *buf, size_t n);

int register_to_all_proc(pid_t pid);
int register_to_proc(sos_proc_t* proc, pid_t pid);
int process_deregister_wait(sos_proc_t* proc, pid_t pid);
int process_wake_waiters(sos_proc_t *proc);
sos_proc_t *select_eviction_process(void);

#endif
