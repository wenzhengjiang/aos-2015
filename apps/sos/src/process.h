/** process.h --- Process management **/

#ifndef _PROCESS_H_
#define _PROCESS_H_

#include <cspace/cspace.h>

#define TEST_PROCESS_NAME             CONFIG_SOS_STARTUP_APP

#define PT_SIZE (1ul << 8)
#define PD_SIZE (1ul << 12)

#define PERM_READ(a) (a & 0b1)
#define PERM_WRITE(a) (a & 0b10)
#define PERM_EXEC(a) (a & 0b100)

typedef struct region {
    seL4_Word start;
    seL4_Word end;
    int perms;
    struct region* next;
} sos_region_t;

typedef struct page_directory_entry {
    seL4_ARM_PageTable sos_pt_cap;
    // cap to in-kernel page table
    seL4_ARM_PageTable pt_cap;
    // ptr to page table
    // TODO: This should NOT be statically allocated
    seL4_Word pt[PT_SIZE];
} sos_pde_t;

typedef struct address_space {
    sos_region_t *regions;
    seL4_Word pd_addr;
    seL4_ARM_PageDirectory pd;
    sos_pde_t pde[PD_SIZE];
} sos_addrspace_t;

typedef struct process {
    sos_addrspace_t vspace;
    seL4_Word ipc_buf_addr;
    seL4_CPtr ipc_buf_cap;
    seL4_Word tcb_addr;
    seL4_TCB tcb_cap;
    cspace_t *cspace;
} sos_proc_t;


int process_create(seL4_CPtr fault_ep);
int process_map_page(sos_proc_t *proc, seL4_Word vaddr);
seL4_Word pt_lookup(sos_proc_t *proc, seL4_Word vaddr);

#endif
