/** process.h --- Process management **/

#ifndef _ADDRSPACE_H_
#define _ADDRSPACE_H_

#include <cspace/cspace.h>
#include <stdbool.h>
#include "swap.h"
#include "sos_type.h"

#define SAVE_PAGE(a) (assert((a << 20) == 0), a >> 12)
#define LOAD_PAGE(a) (assert((a >> 20) == 0), a << 12)

// Allocated using malloc
typedef struct region {
    client_vaddr start;
    client_vaddr end;
    seL4_CapRights rights;
    struct region* next;
    seL4_Word elf_addr;
} sos_region_t;

typedef struct kernel_page_table {
    sos_vaddr addr;
    seL4_ARM_PageTable cap;
    struct kernel_page_table *next;
} kpt_t;

typedef struct page_table_entry {
    seL4_CPtr page_cap;
    struct page_table_entry *next;
    sos_vaddr addr : 24;
    bool refd : 1;
    bool pinned : 1;
    bool swapd  : 1;
} pte_t;

typedef pte_t **pt_t;

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
    size_t pages_mapped;

    pte_t* repllist_head;
    pte_t* repllist_tail;
} sos_addrspace_t;

typedef struct iovec {
    client_vaddr vstart;
    size_t sz;
    struct iovec *next;
    bool sos_iov_flag;
} iovec_t;


sos_region_t* as_region_create(sos_addrspace_t *as, client_vaddr start, client_vaddr end, int rights, seL4_Word elf_addr);
sos_region_t* as_vaddr_region(sos_addrspace_t *as, client_vaddr vaddr);
int as_create(sos_addrspace_t **);
int as_create_page(sos_addrspace_t *as, seL4_Word vaddr, seL4_CapRights rights) ;
sos_vaddr as_lookup_sos_vaddr(sos_addrspace_t *as, client_vaddr vaddr);
void as_activate(sos_addrspace_t* as);
client_vaddr sos_brk(sos_addrspace_t *as, uintptr_t newbrk);
bool is_referenced(sos_addrspace_t *as, client_vaddr vaddr);
bool as_page_exists(sos_addrspace_t *as, client_vaddr vaddr);
int iov_read(iovec_t *, char* buf, int count);
void as_reference_page(sos_addrspace_t *as, client_vaddr vaddr, seL4_CapRights rights);
pte_t* as_lookup_pte(sos_addrspace_t *as, client_vaddr vaddr);
int as_add_page(sos_addrspace_t *as, client_vaddr vaddr, sos_vaddr sos_vaddr);
void as_free(sos_addrspace_t *as);
void unpin_iov(sos_addrspace_t *as, iovec_t *iov);
void as_pin_page(sos_addrspace_t *as, client_vaddr vaddr);
void as_unpin_page(sos_addrspace_t *as, client_vaddr vaddr);

#endif
