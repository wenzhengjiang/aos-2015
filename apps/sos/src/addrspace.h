/** process.h --- Process management **/

#ifndef _ADDRSPACE_H_
#define _ADDRSPACE_H_

#include <cspace/cspace.h>

typedef seL4_Word sos_vaddr;
typedef seL4_Word client_vaddr;

// Allocated using malloc
typedef struct region {
    client_vaddr start;
    client_vaddr end;
    seL4_CapRights rights;
    struct region* next;
} sos_region_t;

typedef struct kernel_page_table {
    sos_vaddr addr;
    seL4_ARM_PageTable cap;
    struct kernel_page_table *next;
} kpt_t;

typedef seL4_Word *pt_t;

// Allocated using malloc
typedef struct address_space {
    sos_region_t *regions;
    sos_region_t *heap_region;
    sos_region_t *stack_region;

    // Allocated using frame_alloc
    pt_t *pd;
    seL4_ARM_PageDirectory sos_pd_cap;
    sos_vaddr sos_pd_addr;
    sos_vaddr sos_ipc_buf_addr;
    kpt_t *kpts;
} sos_addrspace_t;

typedef struct iovec {
    sos_vaddr start;
    size_t sz;
    struct iovec *next;
} iovec_t;


sos_region_t* as_region_create(sos_addrspace_t *as, client_vaddr start, client_vaddr end, int rights);
sos_region_t* as_vaddr_region(sos_addrspace_t *as, client_vaddr vaddr);
int as_create_page(sos_addrspace_t *as, client_vaddr vaddr, seL4_CapRights rights);
sos_addrspace_t* as_create(void);
sos_vaddr as_lookup_sos_vaddr(sos_addrspace_t *as, client_vaddr vaddr);
void as_activate(sos_addrspace_t* as);
client_vaddr sos_brk(sos_addrspace_t *as, uintptr_t newbrk);

int iov_read(iovec_t *, char* buf, int count);
void iov_free(iovec_t *);

#endif
