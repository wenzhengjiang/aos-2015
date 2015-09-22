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
    dprintf(3, "[PR] EVICTING PAGE\n");
    if (!proc->cont.page_replacement_victim) {
        proc->cont.page_replacement_victim = swap_choose_replacement_page(as);
    }
    if (!proc->cont.page_replacement_victim->swapd) {
        victim = proc->cont.page_replacement_victim;
        proc->cont.original_page_addr = LOAD_PAGE(proc->cont.page_replacement_victim->addr);
        printf("Writing: %x, which gets converted to %x\n", victim->addr, LOAD_PAGE(victim->addr));
        printf("Original page addr: %x\n", proc->cont.original_page_addr);
        victim->addr = sos_swap_write(LOAD_PAGE(victim->addr));
        victim->addr = SAVE_PAGE(victim->addr);
        victim->swapd = true;
        longjmp(ipc_event_env, -1);
    }
    if (proc->cont.swap_status == SWAP_SUCCESS) {
        dprintf(4, "[PR] EVICTED. Tidying up.\n");
        assert(!proc->cont.page_replacement_victim->refd);
        proc->cont.page_replacement_victim->valid = false;
        return 0;
    } else {
        longjmp(ipc_event_env, -1);
    }
}

bool swap_is_page_swapped(sos_addrspace_t* as, client_vaddr addr) {
    pte_t *pt = as_lookup_pte(as, addr);
    return pt->swapd;
}

int swap_replace_page(sos_addrspace_t* as, client_vaddr readin) {
    // TODO: Probably need to kill the process.  So much memory contention
    // that we have no room to allocate ANY pages for the new process!
    dprintf(3, "[PR] STARTING PAGE REPLACEMENT\n");
    sos_proc_t *proc = current_process();
    assert(as->repllist_head && as->repllist_tail);
    swap_evict_page(as);
    assert(proc->cont.page_replacement_victim->swapd);
    pte_t *to_load = as_lookup_pte(as, readin);
    if (!proc->cont.page_replacement_request) {
        proc->cont.page_replacement_request = readin;
        if (to_load) {
            dprintf(3, "[PR] READING in targeted replacement page\n");
            assert(proc->cont.original_page_addr != 0);
            memset((void*)proc->cont.original_page_addr, 0, PAGE_SIZE);
            dprintf(4, "[PR] reading in new page %08x from address %u\n", readin, LOAD_PAGE(to_load->addr));
            assert(proc->cont.page_replacement_victim->swapd);
            printf("before read: Original page addr: %x\n", proc->cont.original_page_addr);
            sos_swap_read(proc->cont.original_page_addr, LOAD_PAGE(to_load->addr));
            longjmp(ipc_event_env, -1);
            assert(proc->cont.original_page_addr != 0);
        } else {
            assert(!"Did not find page");
        }
    }
    if (proc->cont.swap_status == SWAP_SUCCESS) {
        dprintf(3, "[PR] REPLACEMENT COMPLETE\n");
        printf("after: Original page addr: %x\n", proc->cont.original_page_addr);
        to_load->addr = SAVE_PAGE(proc->cont.original_page_addr);
        to_load->swapd = false;
        seL4_CPtr fc = frame_cap(LOAD_PAGE(to_load->addr));
        seL4_ARM_Page_Unify_Instruction(fc, 0, PAGE_SIZE);
        //printf("NEW PAGE LOADED OKAY\n");
        return 0;
    } else {
        longjmp(ipc_event_env, -1);
    }
}
