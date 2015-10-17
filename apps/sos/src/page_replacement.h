#ifndef _SOS_PAGE_REPLACEMENT_H_
#define _SOS_PAGE_REPLACEMENT_H_

#include "process.h"
#include "addrspace.h"

int swap_in_page(client_vaddr target);
int swap_evict_page(sos_proc_t *as);
bool swap_is_page_swapped(sos_addrspace_t* as, client_vaddr addr);

#endif
