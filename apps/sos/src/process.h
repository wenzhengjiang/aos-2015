/** process.h --- Process management **/

#ifndef _PROCESS_H_
#define _PROCESS_H_

#include <nfs/nfs.h>
#include <cspace/cspace.h>
#include "addrspace.h"
#include "file.h"

#define TEST_PROCESS_NAME             CONFIG_SOS_STARTUP_APP

typedef struct continuation {
    seL4_CPtr reply_cap;
    int fd;
    int counter;
    iovec_t* iov;
    int target;
    char *filename;
}cont_t;

typedef struct process {
    int pid;
    sos_addrspace_t *vspace;
    seL4_Word tcb_addr;
    seL4_TCB tcb_cap;
    cspace_t *cspace;
    seL4_CPtr user_ep_cap;
    fd_table_t fd_table;
    cont_t cont;
} sos_proc_t;

int process_create(seL4_CPtr fault_ep);
sos_addrspace_t *proc_as(sos_proc_t *proc);
sos_addrspace_t *current_as(void);
sos_proc_t *current_process(void);
sos_proc_t *process_lookup(pid_t pid);
of_entry_t *fd_lookup(sos_proc_t *proc, int fd);
void fd_free(sos_proc_t* proc, int fd);

#endif
