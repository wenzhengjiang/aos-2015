/** process.h --- Process management **/

#ifndef _ADDRSPACE_H_
#define _ADDRSPACE_H_

#include <cspace/cspace.h>

#define PERM_READ(a) (a & 1ul)
#define PERM_WRITE(a) (a & (1ul << 1))
#define PERM_EXEC(a) (a & (1ul << 2))

// Allocated using malloc
typedef struct region {
    seL4_Word start;
    seL4_Word end;
    seL4_CapRights perms;
    struct region* next;
} sos_region_t;

// Allocated using malloc
typedef struct page_directory_entry {
    // Allocated using frame_alloc
    seL4_Word *pt;
    seL4_Word sos_pt_addr;
    seL4_ARM_PageTable sos_pt_cap;
} sos_pde_t;

typedef sos_pde_t *sos_pd_t;

// Allocated using malloc
typedef struct address_space {
    sos_region_t *regions;
    sos_region_t *heap_region;
    sos_region_t *stack_region;
    sos_region_t *ipc_region;

    size_t nregions;
    // Allocated using frame_alloc
    sos_pd_t *pd;
    seL4_ARM_PageDirectory sos_pd_cap;
    seL4_Word sos_pd_addr;
    seL4_Word sos_ipc_buf_addr;
    seL4_Word sos_stack_addr;
    seL4_Word sos_heap_addr;
} sos_addrspace_t;

sos_region_t* as_region_create(sos_addrspace_t *as, seL4_Word start, seL4_Word end, int rights);
sos_region_t* as_vaddr_region(sos_addrspace_t *as, seL4_Word vaddr);
int as_map_page(sos_addrspace_t *as, seL4_Word vaddr, seL4_Word *sos_vaddr);
sos_addrspace_t* as_create(void);
seL4_Word as_page_lookup(sos_addrspace_t *as, seL4_Word vaddr);
void init_essential_regions(sos_addrspace_t* as);
void map_essential_regions(sos_addrspace_t* as);
seL4_Word brk(sos_addrspace_t *as, uintptr_t newbrk);

#endif
