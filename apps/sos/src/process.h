/** process.h --- Process management **/

#ifndef _PROCESS_H_
#define _PROCESS_H_

#define PT_SIZE (1ul << 8)
#define PD_SIZE (1ul << 11)

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
    // cap to in-kernel page table
    seL4_CPtr pt_cap;
    // ptr to page table
    seL4_CPtr frame_cap[PT_SIZE];
} sos_pde_t;

typedef struct address_space {
    sos_region_t *regions;
    seL4_CPtr pd_cap;
    sos_pde_t pde[PD_SIZE];
} sos_addrspace_t;

typedef struct process {
    sos_addrspace_t vspace;
} sos_proc_t;

#endif
