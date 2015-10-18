/**
 * @file page_replacement.c
 * @brief Implementation of page replacement algorithm
 */
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include "frametable.h"
#include "addrspace.h"
#include "page_replacement.h"
#include "process.h"
#include "syscall.h"
#include "swap.h"
#include "handler.h"
#include <assert.h>

#define verbose 0
#include <log/debug.h>
#include <log/panic.h>

extern size_t addrspace_pages;

/**
 * Second chance page replacement algorithm.  Maintains a ref bit in the PTE,
 * which tracks whether the page has been referenced since the last time the
 * algorithm was invoked.  Evicted pages are unmapped from the process address
 * space.
 * @param as The address space for searching for victim pages
 * @return PTE of the selected victim
 */
static pte_t* swap_choose_replacement_page(sos_addrspace_t* as) {
    assert(as);
    pte_t* head = as->repllist_head;
    int err;
    int loop_count = 0;
    while(1) {
        if (head == as->repllist_head) {
            if (loop_count > 1) { // all pages are pinned or swaped 
                dprintf(1, "[PR] No pages left for eviction. Invoking 'OOM killer'\n");
                if (effective_process() != current_process()) {
                    WARN("Failed to start the new process");
                    syscall_end_continuation(current_process(), 0, false);
                }
                process_delete(effective_process());
                longjmp(ipc_event_env, -1);
            }
            loop_count++;
        }
        dprintf(4, "tick\n");
        if(as->repllist_head->pinned || as->repllist_head->swapd) {
            as->repllist_tail = as->repllist_head;
            as->repllist_head = as->repllist_head->next;
            continue;
        }
        /*If reference bit is on, turn it off and unmap the page, so we can 
         * turn it out in vm_fault handler*/
        if(as->repllist_head->refd) {
            as->repllist_tail = as->repllist_head;
            as->repllist_head = as->repllist_head->next;
            seL4_ARM_Page_Unmap(as->repllist_tail->page_cap); 
            cspace_revoke_cap(cur_cspace, as->repllist_tail->page_cap);
            err = cspace_delete_cap(cur_cspace, as->repllist_tail->page_cap);
            if (err != CSPACE_NOERROR) {
                ERR("[PR]: failed to delete page cap\n");
            }
            as->repllist_tail->page_cap = 0;
            as->repllist_tail->refd = false;
        } else {
            as->repllist_tail = as->repllist_head;
            as->repllist_head = as->repllist_head->next;
            return as->repllist_tail;
        }
    }
    assert(!"This can never happen");
}

/**
 * Evict a page from the address space, swapping it out to 'disk'
 * @param as Address space from which to evict
 * @return Zero if successful, non-zero otherwise.
 */
int swap_evict_page(sos_proc_t *evict_proc) {
    dprintf(3, "[PR] EVICTING PAGE\n");
    sos_proc_t *proc = current_process();
    assert(proc);
    if (!proc->cont.page_replacement_victim) {
        proc->cont.page_replacement_victim = swap_choose_replacement_page(evict_proc->vspace);
    }

    // Continuation
    if (!proc->cont.swap_write_fired) {
        addrspace_pages--;
        evict_proc->vspace->pages_mapped--;
        proc->cont.page_replacement_victim->pinned = true;
        proc->cont.original_page_addr = LOAD_PAGE((seL4_Word)proc->cont.page_replacement_victim->addr);

        sos_swap_write(proc->cont.original_page_addr);
        // Wait on network irq
        longjmp(ipc_event_env, -1);
    }

    // Continuation
    if (proc->cont.swap_status == SWAP_SUCCESS) {
        dprintf(4, "[PR] EVICTED. Tidying up.\n");
        dprintf(4, "victim: %p\n", proc->cont.page_replacement_victim);
        assert(proc->cont.page_replacement_victim);
        if (proc->cont.page_replacement_victim->refd) {
            // Process has accessed in the interim
            seL4_ARM_Page_Unmap(proc->cont.page_replacement_victim->page_cap);
            cspace_revoke_cap(cur_cspace, proc->cont.page_replacement_victim->page_cap);
            int err = cspace_delete_cap(cur_cspace, proc->cont.page_replacement_victim->page_cap);
            if (err != CSPACE_NOERROR) {
                ERR("[PR]: failed to delete page cap\n");
            }
            proc->cont.page_replacement_victim->page_cap = 0;
            proc->cont.page_replacement_victim->refd = false;
        }

        proc->cont.page_replacement_victim->addr = SAVE_PAGE(proc->cont.swap_file_offset);
        proc->cont.page_replacement_victim->swapd = true;
        proc->cont.page_replacement_victim->pinned = false;
        proc->cont.swap_write_fired = false;
        proc->cont.swap_status = 0;
        return 0;
    } else if (proc->cont.swap_status == SWAP_FAILED) {
        ERR("[PR] Deleting process due to swap failure\n");
        if (effective_process() != current_process()) {
            syscall_end_continuation(current_process(), -1, false);
        }
        process_delete(effective_process());
        longjmp(ipc_event_env, -1);
    } else {
        dprintf(4, "[PR] Jumping back\n");
        // Wait on network irq
        longjmp(ipc_event_env, -1);
    }
}

/**
 * Predicate indicating whether a particular address in an address space is swaped.
 * @param as the address space to check
 * @param addr the virtual address in that address space.
 * @return True if page containing addr is on disk, false otherwise.
 */
bool swap_is_page_swapped(sos_addrspace_t* as, client_vaddr addr) {
    pte_t *pt = as_lookup_pte(as, addr);
    return pt->swapd;
}

/**
 * @brief   read a page from swap file to memory
 *          It needs to allocate a new frame or swap out another page 
 *
 * @param readin
 *
 * @return 
 */
int swap_in_page(client_vaddr readin) {
    dprintf(3, "[PR] STARTING PAGE REPLACEMENT\n");
    sos_proc_t* proc = current_process();
    sos_addrspace_t *as = proc->vspace;

    if (!proc->cont.have_new_frame) {
        assert(as->repllist_head && as->repllist_tail);
        seL4_Word tmp;
        if(frame_alloc(&tmp) == 0) {
            ERR("Failed to make frame for page to swap in");
            process_delete(current_process());
        }
        proc->cont.have_new_frame = true;
        proc->cont.original_page_addr = tmp;
        dprintf(3, "[PR] Finished eviction, new frame = %08x\n", proc->cont.original_page_addr);
        dprintf(3, "[PR] proc %p, victim: %p\n", proc, proc->cont.page_replacement_victim);
    }

    dprintf(3, "[PR] replacement request %x\n", proc->cont.page_replacement_request);
    if (proc->cont.page_replacement_request == 0) {
        proc->cont.page_replacement_request = readin;
        pte_t *to_load = as_lookup_pte(as, proc->cont.page_replacement_request);
        assert(to_load);
        assert(to_load->swapd);

        dprintf(3, "[PR] READING in targeted replacement page\n");
        assert(proc->cont.original_page_addr);
        memset((void*)proc->cont.original_page_addr, 0, PAGE_SIZE);
        dprintf(4, "[PR] reading in new page %08x from address %u\n", readin, LOAD_PAGE(to_load->addr));

        sos_swap_read(proc->cont.original_page_addr, LOAD_PAGE(to_load->addr));
        longjmp(ipc_event_env, -1);
    }
    if (proc->cont.swap_status == SWAP_SUCCESS) {
        pte_t *to_load = as_lookup_pte(as, proc->cont.page_replacement_request);
        assert(to_load);
        assert(to_load->swapd);

        dprintf(3, "[PR] REPLACEMENT COMPLETE\n");
        to_load->addr = SAVE_PAGE(proc->cont.original_page_addr);
        assert(to_load->pinned == false);
        addrspace_pages++;
        as->pages_mapped++;
        to_load->swapd = false;
        dprintf(3, "[PR] OPR: %x\n", proc->cont.original_page_addr);
        seL4_CPtr fc = frame_cap(proc->cont.original_page_addr);
        seL4_ARM_Page_Unify_Instruction(fc, 0, PAGE_SIZE);
        proc->cont.swap_status = 0;
        proc->cont.page_replacement_request = 0;
        proc->cont.original_page_addr = 0;
        proc->cont.page_replacement_victim = NULL;
        proc->cont.have_new_frame = false;
        return 0;
    } else {
        longjmp(ipc_event_env, -1);
    }
}
