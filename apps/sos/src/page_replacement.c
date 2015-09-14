#include <sel4/sel4.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include "frametable.h"
#include "addrspace.h"
#include "page_replacement.h"
#include "process.h"
#include "swap.h"
#include "handler.h"
#include <assert.h>

#define verbose 5
#include <log/debug.h>
#include <log/panic.h>

static pte_t* swap_choose_replacement_page(sos_addrspace_t* as) {
    while(1) {
        if(as->repllist_head->refd) {
            as->repllist_tail = as->repllist_head;
            as->repllist_head = as->repllist_head->next;
            as->repllist_tail->refd = false;
        } else {
            as->repllist_tail = as->repllist_head;
            as->repllist_head = as->repllist_head->next;
            return as->repllist_tail;
        }
    }
    assert(!"This can never happen");
}

int swap_evict_page(sos_addrspace_t *as) {
    sos_proc_t *proc = current_process();
    pte_t *victim;
    printf("EVICTING\n");
    if (!proc->cont.page_replacement_victim) {
        proc->cont.page_replacement_victim = swap_choose_replacement_page(as);
    }
    if (proc->cont.page_replacement_victim->swaddr == (unsigned)-1) {
        victim = proc->cont.page_replacement_victim;
        printf("evict about to call SSW\n");
        victim->swaddr = sos_swap_write(victim->addr);
        printf("SSW returned.  Jumping.\n");
        longjmp(ipc_event_env, -1);
    }
    return 0;
}

bool swap_is_page_swapped(sos_addrspace_t* as, client_vaddr addr) {
    pte_t *pt = as_lookup_pte(as, addr);
    return (pt->swaddr != (unsigned)-1);
}

int swap_replace_page(sos_addrspace_t* as, client_vaddr readin) {
    // TODO: Probably need to kill the process.  So much memory contention
    // that we have no room to allocate ANY pages for the new process!
    printf("REPLACING\n");
    sos_proc_t *proc = current_process();
    assert(as->repllist_head && as->repllist_tail);
    if (proc->cont.page_replacement_victim && proc->cont.page_replacement_victim->swaddr == (unsigned)-1) {
        pte_t* victim = swap_choose_replacement_page(as);
        proc->cont.page_replacement_victim = victim;
        victim->swaddr = sos_swap_write(victim->addr);
        longjmp(ipc_event_env, -1);
    }

    memset((void*)proc->cont.page_replacement_victim->addr, 0, PAGE_SIZE);
    seL4_ARM_Page_Unmap(proc->cont.page_replacement_victim->page_cap);
    cspace_delete_cap(cur_cspace, proc->cont.page_replacement_victim->page_cap);
    assert(proc->cont.page_replacement_victim->refd == false);

    pte_t *to_load = as_lookup_pte(as, readin);
    if (!proc->cont.page_replacement_request) {
        proc->cont.page_replacement_request = readin;
        if (to_load) {
            sos_swap_read(proc->cont.page_replacement_victim->addr, to_load->swaddr);
            longjmp(ipc_event_env, -1);
        } else {
            assert(!"Did not find page");
        }
    }
    to_load->swaddr = (unsigned)-1;
    return 0;
}
