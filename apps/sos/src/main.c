/*
 * Copyright 2014, NICTA
 *
 * This software may be distributed and modified according to the terms of
 * the BSD 2-Clause license. Note that NO WARRANTY is provided.
 * See "LICENSE_BSD2.txt" for details.
 *
 * @TAG(NICTA_BSD)
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <string.h>

#include <cspace/cspace.h>
#include <setjmp.h>

#include <cpio/cpio.h>
#include <nfs/nfs.h>
#include <elf/elf.h>
#include <serial/serial.h>
#include <clock/clock.h>
#include <limits.h>
#include <sel4/sel4.h>

#include "frametable.h"
#include "process.h"
#include "addrspace.h"
#include "syscall.h"
#include "handler.h"
#include "serial.h"

#include "network.h"
#include "elf.h"
#include "sos_nfs.h"
#include "swap.h"

#include <device/mapping.h>
#include <syscallno.h>

#include <ut/ut.h>
#include <device/vmem_layout.h>

#include <autoconf.h>
#include <errno.h>

#define verbose 5
#include <log/debug.h>
#include <log/panic.h>

#include <sync/mutex.h>

/* To differencient between async and and sync IPC, we assign a
 * badge to the async endpoint. The badge that we receive will
 * be the bitwise 'OR' of the async endpoint badge and the badges
 * of all pending notifications. */
#define IRQ_EP_BADGE         (1 << (seL4_BadgeBits - 1))
/* All badged IRQs set high bet, then we use uniq bits to
 * distinguish interrupt sources */
#define IRQ_BADGE_NETWORK (1 << 0)
#define IRQ_BADGE_CLOCK (1 << 1)

/* The linker will link this symbol to the start address  *
 * of an archive of attached applications.                */
const seL4_BootInfo* _boot_info;

jmp_buf ipc_event_env; // jmp_buf for main event loop

seL4_CPtr _sos_ipc_ep_cap;
seL4_CPtr _sos_interrupt_ep_cap;

/**
 * @brief Main event loop of sos
 *
 * @param ep endpoints where to receive triggered events
 */
void syscall_loop(seL4_CPtr ep) {
    sos_proc_t *proc = NULL;
    static bool bootstrapped = false;
    static bool bootstrap_init = false;
    register_handlers();
    int pid = setjmp(ipc_event_env);
    if (!bootstrap_init) {
        bootstrap_init = true;
        int err = start_process(TEST_PROCESS_NAME, _sos_ipc_ep_cap);
        if (err) {
            dprintf(0, "Failed to start the first client\n");
            return ;
        }
        assert(current_process()); // current process here should be set to the first process started 
        memset(&current_process()->cont, 0, sizeof(cont_t));
        bootstrapped = true;
    }
    while (1) {
        dprintf(4, "[MAIN] Restart syscall loop\n");
        seL4_Word badge = 0;
        seL4_Word label;
        seL4_MessageInfo_t message;
/***** Prepare events and set up current process *****/
        if (has_ready_proc()) { 
            /*1. Pick one resumed execution and continue it*/
            dprintf(4, "[MAIN] Applying continuation\n");
            pid = next_ready_proc();
            set_current_process(pid);
            proc = process_lookup(pid);

            label = proc->cont.ipc_label;
        } else if (pid < -1) { 
            /*sos got fatal error event, we quit*/
            ERR("Fatal error happened in sos (error code : %d)!\n", pid);
            break;
        } else {
            /*Wait event sent via endpoint (could be IPC, network or clock ...)*/
            dprintf(4, "[MAIN] New continuation\n");
            message = seL4_Wait(ep, &badge);
            label = seL4_MessageInfo_get_label(message);
            if (badge < MAX_PROCESS_NUM) {   // Check whether it is a IPC request from a client
                set_current_process((int)badge);
                proc = current_process();
                dprintf(4, "[MAIN] Received %u from process\n", proc->pid);
            }
        }
/***** Handle received events *****/
        if(badge & IRQ_EP_BADGE){
            /*1. Clock interrupts*/
            if (badge &  IRQ_BADGE_CLOCK) {
                dprintf(4, "[MAIN] Starting timer interrupt\n");
                timer_interrupt();
            }
            /*2. Network interrupts (might be IO interrupts)*/
            if (badge & IRQ_BADGE_NETWORK) {
                dprintf(4, "[MAIN] Starting network interrupt\n");
                /* All NFS callbacks are executed in network_irq(). 
                 * Track them by putting corresponding pid into wait_proc_queue.
                 * So we can continue executions which fired those callbacks.*/
                network_irq(); 
                dprintf(4, "[MAIN] Leaving network interrupt\n");
            }
        } else if (pid > 0 && !bootstrapped) {
            /*3. Starting process of the first client was resumed*/
            assert(pid == 1); // First client has pid 1
            start_process(TEST_PROCESS_NAME, _sos_ipc_ep_cap);
            memset(&current_process()->cont, 0, sizeof(cont_t));
            bootstrapped = true;
        } else if(label == seL4_VMFault){
            /*4. An client caused page fault */
            if (!pid || !proc->cont.syscall_loop_initiations) {
                dprintf(4, "vm fault at 0x%08x, pc = 0x%08x, %s\n", seL4_GetMR(1),
                        seL4_GetMR(0),
                        seL4_GetMR(2) ? "Instruction Fault" : "Data fault");
            }
            if (proc->cont.syscall_loop_initiations == 0) { // Initialize continuation
                proc->cont.vm_fault_type = seL4_GetMR(3);
                proc->cont.client_addr = seL4_GetMR(1);
                proc->cont.ipc_label = seL4_VMFault;
                proc->cont.reply_cap = cspace_save_reply_cap(cur_cspace);
            }
            proc->cont.syscall_loop_initiations++;
            int err = sos_vm_fault(proc->cont.vm_fault_type, proc->cont.client_addr);
            if (err) {
                process_delete(proc);
                dprintf(0, "vm_fault couldn't be handled, process is killed %d \n", err);
            } else {
                syscall_end_continuation(proc, 0, true); // reboot the client
            }
        } else if(label == seL4_NoFault) {
            /*5. Syscall requests from clients*/
            if (proc->cont.syscall_loop_initiations == 0) { // Initialize continuation
                dprintf(4, "[MAIN] Starting syscall\n");
                proc->cont.syscall_number = seL4_GetMR(0);
                proc->cont.reply_cap = cspace_save_reply_cap(cur_cspace);
                proc->cont.ipc_label = label;
            } else {
                dprintf(4, "[MAIN] Restarting syscall %d\n", proc->cont.syscall_loop_initiations);
            }
            proc->cont.syscall_loop_initiations++;
            /* Execute System call */
            handle_syscall(proc->cont.syscall_number);
            dprintf(0, "handle_syscall end\n");
        }else{
            ERR("Rootserver got an unknown message\n");
        }
        pid = 0;
    }
}

static void print_bootinfo(const seL4_BootInfo* info) {
    int i;

    /* General info */
    dprintf(1, "Info Page:  %p\n", info);
    dprintf(1,"IPC Buffer: %p\n", info->ipcBuffer);
    dprintf(1,"Node ID: %d (of %d)\n",info->nodeID, info->numNodes);
    dprintf(1,"IOPT levels: %d\n",info->numIOPTLevels);
    dprintf(1,"Init cnode size bits: %d\n", info->initThreadCNodeSizeBits);

    /* Cap details */
    dprintf(1,"\nCap details:\n");
    dprintf(1,"Type              Start      End\n");
    dprintf(1,"Empty             0x%08x 0x%08x\n", info->empty.start, info->empty.end);
    dprintf(1,"Shared frames     0x%08x 0x%08x\n", info->sharedFrames.start, 
            info->sharedFrames.end);
    dprintf(1,"User image frames 0x%08x 0x%08x\n", info->userImageFrames.start, 
            info->userImageFrames.end);
    dprintf(1,"User image PTs    0x%08x 0x%08x\n", info->userImagePTs.start, 
            info->userImagePTs.end);
    dprintf(1,"Untypeds          0x%08x 0x%08x\n", info->untyped.start, info->untyped.end);

    /* Untyped details */
    dprintf(1,"\nUntyped details:\n");
    dprintf(1,"Untyped Slot       Paddr      Bits\n");
    for (i = 0; i < info->untyped.end-info->untyped.start; i++) {
        dprintf(1,"%3d     0x%08x 0x%08x %d\n", i, info->untyped.start + i,
                info->untypedPaddrList[i],
                info->untypedSizeBitsList[i]);
    }

    /* Device untyped details */
    dprintf(1,"\nDevice untyped details:\n");
    dprintf(1,"Untyped Slot       Paddr      Bits\n");
    for (i = 0; i < info->deviceUntyped.end-info->deviceUntyped.start; i++) {
        dprintf(1,"%3d     0x%08x 0x%08x %d\n", i, info->deviceUntyped.start + i,
                info->untypedPaddrList[i + (info->untyped.end - info->untyped.start)],
                info->untypedSizeBitsList[i + (info->untyped.end-info->untyped.start)]);
    }
}


static void _sos_ipc_init(seL4_CPtr* ipc_ep, seL4_CPtr* async_ep){
    seL4_Word ep_addr, aep_addr;
    int err;

    /* Create an Async endpoint for interrupts */
    aep_addr = ut_alloc(seL4_EndpointBits);
    conditional_panic(!aep_addr, "No memory for async endpoint");
    err = cspace_ut_retype_addr(aep_addr,
            seL4_AsyncEndpointObject,
            seL4_EndpointBits,
            cur_cspace,
            async_ep);
    conditional_panic(err, "Failed to allocate c-slot for Interrupt endpoint");

    /* Bind the Async endpoint to our TCB */
    err = seL4_TCB_BindAEP(seL4_CapInitThreadTCB, *async_ep);
    conditional_panic(err, "Failed to bind ASync EP to TCB");

    /* Create an endpoint for user application IPC */
    ep_addr = ut_alloc(seL4_EndpointBits);
    conditional_panic(!ep_addr, "No memory for endpoint");
    err = cspace_ut_retype_addr(ep_addr, 
            seL4_EndpointObject,
            seL4_EndpointBits,
            cur_cspace,
            ipc_ep);
    conditional_panic(err, "Failed to allocate c-slot for IPC endpoint");
}

static inline seL4_CPtr badge_irq_ep(seL4_CPtr ep, seL4_Word badge) {
    seL4_CPtr badged_cap = cspace_mint_cap(cur_cspace, cur_cspace, ep, seL4_AllRights, seL4_CapData_Badge_new(badge | IRQ_EP_BADGE));
    conditional_panic(!badged_cap, "Failed to allocate badged cap");
    return badged_cap;
}

/**
 * @brief Initialize sos at booting time
 *
 */
static void _sos_init(seL4_CPtr* ipc_ep, seL4_CPtr* async_ep){
    seL4_Word dma_addr;
    seL4_Word low, high;
    int err;

    /* Retrieve boot info from seL4 */
    _boot_info = seL4_GetBootInfo();
    conditional_panic(!_boot_info, "Failed to retrieve boot info\n");
    if(verbose > 0){
        print_bootinfo(_boot_info);
    }

    /* Initialize the untyped sub system and reserve memory for DMA */
    err = ut_table_init(_boot_info);
    conditional_panic(err, "Failed to initialize Untyped Table\n");
    /* DMA uses a large amount of memory that will never be freed */
    dma_addr = ut_steal_mem(DMA_SIZE_BITS);
    conditional_panic(dma_addr == 0, "Failed to reserve DMA memory\n");

    /* find available memory */
    ut_find_memory(&low, &high);

    /* Initialize the untyped memory allocator */
    ut_allocator_init(low, high);

    /* Initialize the cspace manager */
    err = cspace_root_task_bootstrap(ut_alloc, ut_free, ut_translate,
            malloc, free);
    conditional_panic(err, "Failed to initialize the cspace\n");

    /* Initialize DMA memory */
    err = dma_init(dma_addr, DMA_SIZE_BITS);
    conditional_panic(err, "Failed to initialize DMA memory\n");

    /* Initialize IPC */
    _sos_ipc_init(ipc_ep, async_ep);

    /* Initialize the network hardware */
    network_init(badge_irq_ep(*async_ep, IRQ_BADGE_NETWORK));

    /* Initialize and start the clock driver */
    start_timer(badge_irq_ep(*async_ep, IRQ_BADGE_CLOCK));

    /* Initialize frame table and swap table*/
    frame_init();

    /* Initialize nfs */
    sos_nfs_init(CONFIG_SOS_NFS_DIR);

    /* Initialize serial device*/
    sos_serial_init();

}

/*
 * Main entry point - called by crt.
 */
int main(void) {

    dprintf(0, "\nSOS Starting...\n");

    _sos_init(&_sos_ipc_ep_cap, &_sos_interrupt_ep_cap);

    
    /* Wait on synchronous endpoint for IPC */
    dprintf(-1, "\nSOS entering syscall loop\n");
    syscall_loop(_sos_ipc_ep_cap);
    srand(26706);
    printf("game over!\n");
    while(1) {  }

    /* Not reached */
    return 0;
}
