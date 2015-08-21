/** process.h --- Process management **/

#ifndef _PROCESS_H_
#define _PROCESS_H_

#include <cspace/cspace.h>
#include "addrspace.h"

#define TEST_PROCESS_NAME             CONFIG_SOS_STARTUP_APP

typedef struct process {
    sos_addrspace_t *vspace;
    seL4_Word tcb_addr;
    seL4_TCB tcb_cap;
    cspace_t *cspace;
    seL4_CPtr user_ep_cap;
} sos_proc_t;


int process_create(seL4_CPtr fault_ep);
sos_addrspace_t *proc_as(sos_proc_t *proc);
sos_proc_t *current_process(void);

#endif
