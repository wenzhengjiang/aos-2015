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
        dprintf(4, "[PRCLOCK] Tick\n");
        if(!as->repllist_head->valid) {
            as->repllist_tail = as->repllist_head;
            as->repllist_head = as->repllist_head->next;
            // TODO: There's potential for a hang here.  We should detect &
            // kill client if it ends up with too many pages pinned, as we
            // can't guarantee consistency should we start forcefully
            // unpinning pages.
            continue;
        }
        if(as->repllist_head->refd) {
            as->repllist_tail = as->repllist_head;
            as->repllist_head = as->repllist_head->next;
            seL4_ARM_Page_Unmap(as->repllist_tail->page_cap);
            cspace_delete_cap(cur_cspace, as->repllist_tail->page_cap);
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
    printf("STARTING EVICTION\n");
    if (!proc->cont.page_replacement_victim) {
        proc->cont.page_replacement_victim = swap_choose_replacement_page(as);
    }
    if (proc->cont.page_replacement_victim->swaddr == (unsigned)-1) {
        victim = proc->cont.page_replacement_victim;
        victim->swaddr = sos_swap_write(victim->addr);
        longjmp(ipc_event_env, -1);
    }
    if (proc->cont.swap_status == SWAP_SUCCESS) {
        dprintf(4, "[PR] Evicted. Tidying up.\n");
        assert(!proc->cont.page_replacement_victim->refd);
        proc->cont.page_replacement_victim->valid = false;
        return 0;
    } else {
        longjmp(ipc_event_env, -1);
    }
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
    swap_evict_page(as);
    assert(proc->cont.page_replacement_victim->swaddr != -1);
    pte_t *to_load = as_lookup_pte(as, readin);
    if (!proc->cont.page_replacement_request) {
        proc->cont.page_replacement_request = readin;
        if (to_load) {
            assert(proc->cont.page_replacement_victim->addr != 0);
            memset((void*)proc->cont.page_replacement_victim->addr, 0, PAGE_SIZE);
            printf("reading in new page %08x from address %u\n", readin, to_load->swaddr);
            to_load->addr = proc->cont.page_replacement_victim->addr;
            sos_swap_read(proc->cont.page_replacement_victim->addr, to_load->swaddr);
            longjmp(ipc_event_env, -1);
            assert(proc->cont.page_replacement_victim->addr != 0);
        } else {
            assert(!"Did not find page");
        }
    }
    if (proc->cont.swap_status == SWAP_SUCCESS) {
        to_load->swaddr = (unsigned)-1;
        seL4_CPtr fc = frame_cap(to_load->addr);
        seL4_ARM_Page_Unify_Instruction(fc, 0, PAGE_SIZE);
        //printf("NEW PAGE LOADED OKAY\n");
        return 0;
    } else {
        longjmp(ipc_event_env, -1);
    }
}
