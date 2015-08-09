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
#include <assert.h>
#include <string.h>

#include <cspace/cspace.h>

#include <cpio/cpio.h>
#include <nfs/nfs.h>
#include <elf/elf.h>
#include <serial/serial.h>
#include <clock/clock.h>

#include "network.h"
#include "elf.h"

#include "ut_manager/ut.h"
#include "vmem_layout.h"
#include "mapping.h"

#include <autoconf.h>

#define verbose 5
#include <sys/debug.h>
#include <sys/panic.h>

#include <sync/mutex.h>

/* This is the index where a clients syscall enpoint will
 * be stored in the clients cspace. */
#define USER_EP_CAP          (1)
/* To differencient between async and and sync IPC, we assign a
 * badge to the async endpoint. The badge that we receive will
 * be the bitwise 'OR' of the async endpoint badge and the badges
 * of all pending notifications. */
#define IRQ_EP_BADGE         (1 << (seL4_BadgeBits - 1))
/* All badged IRQs set high bet, then we use uniq bits to
 * distinguish interrupt sources */
#define IRQ_BADGE_NETWORK (1 << 0)
#define IRQ_BADGE_CLOCK (1 << 1)

#define TTY_NAME             CONFIG_SOS_STARTUP_APP
#define TTY_PRIORITY         (0)
#define TTY_EP_BADGE         (101)

/* The linker will link this symbol to the start address  *
 * of an archive of attached applications.                */
extern char _cpio_archive[];

const seL4_BootInfo* _boot_info;


struct {
    seL4_Word tcb_addr;
    seL4_TCB tcb_cap;

    seL4_Word vroot_addr;
    seL4_ARM_PageDirectory vroot;

    seL4_Word ipc_buffer_addr;
    seL4_CPtr ipc_buffer_cap;

    cspace_t *croot;

} tty_test_process;


/*
 * A dummy starting syscall
 */
#define SOS_SYSCALL0 0

/* syscall for printing */
#define SOS_SYSCALL_PRINT 2
#define PRINT_MESSAGE_START 2

seL4_CPtr _sos_ipc_ep_cap;
seL4_CPtr _sos_interrupt_ep_cap;

bool timer_stop = false;

/**
 * NFS mount point
 */
extern fhandle_t mnt_point;

static inline int CONST min(int a, int b)
{
    return (a < b) ? a : b;
}

/**
 * Use syscall_print_sendbuf to send the buffer
 * @param msgBuf message buffer
 * @param length of content to send
 */
static size_t
syscall_print_sendbuf(char* msgBuf, size_t length) {
    struct serial* serial = serial_init();
    // Length should be no more than standard MTU
    assert(length <= 1500);
    serial_send(serial, msgBuf, length);
    return length;
}

/**
 * Unpack characters from seL4_Words.  First char is most sig. 8 bits.
 * @param msgBuf starting point of buffer to store contents.  Must have at
 * least 4 chars available.
 * @param packed_data word packed with 4 characters
 */
static int unpack_word(char* msgBuf, seL4_Word packed_data) {
    int length = 0;
    int j = sizeof(seL4_Word);
    while (j > 0) {
        // Unpack data encoded 4-chars per word.
        *msgBuf = (char)(packed_data >> ((--j) * 8));
        if (*msgBuf == 0) {
            return length;
        }
        length++;
        msgBuf++;
    }
    return length;
}

/**
 * @param num_args number of IPC args supplied
 * @returns length of the message printed
 */
static size_t syscall_print(size_t num_args) {
    size_t i,unpack_len,send_len;
    size_t total_unpack = 0;
    seL4_Word packed_data;
    char *msgBuf = malloc(seL4_MsgMaxLength * sizeof(seL4_Word));
    char *bufPtr = msgBuf;
    char req_count = seL4_GetMR(1);
    memset(msgBuf, 0, seL4_MsgMaxLength * sizeof(seL4_Word));
    for (i = 0; i < num_args - PRINT_MESSAGE_START + 1; i++) {
        packed_data = seL4_GetMR(i + PRINT_MESSAGE_START);
        unpack_len = unpack_word(bufPtr, packed_data);
        total_unpack += unpack_len;
        bufPtr += unpack_len;
        /* Unpack was short the expected amount, so we signal the end. */
        if (unpack_len < sizeof(seL4_Word)) {
            break;
        }
    }
    send_len = syscall_print_sendbuf(msgBuf, min(req_count, total_unpack));
    free(msgBuf);
    return send_len;
}

static int timer_init(seL4_CPtr interrupt_ep) {
    void* gpt_clock_addr = map_device((void*)CLOCK_GPT_PADDR, sizeof(gpt_register_t));
    clock_set_device_address(gpt_clock_addr);
    start_timer(interrupt_ep);
    return 0;
}

void handle_syscall(seL4_Word badge, int num_args) {
    seL4_Word syscall_number;
    seL4_CPtr reply_cap;
    seL4_MessageInfo_t reply;
    seL4_Word reply_msg;

    syscall_number = seL4_GetMR(0);

    /* Save the caller */
    reply_cap = cspace_save_reply_cap(cur_cspace);
    assert(reply_cap != CSPACE_NULL);

    /* Process system call */
    switch (syscall_number) {
    case SOS_SYSCALL0:
        dprintf(0, "syscall: thread made syscall 0!\n");

        reply = seL4_MessageInfo_new(0, 0, 0, 1);
        seL4_SetMR(0, 0);
        seL4_Send(reply_cap, reply);
        break;
    case SOS_SYSCALL_PRINT:
        reply_msg = syscall_print(num_args);
        reply = seL4_MessageInfo_new(0, 0, 0, 1);
        seL4_SetMR(0, 0);
        seL4_SetMR(1, reply_msg);
        seL4_Send(reply_cap, reply);
        break;
    default:
        printf("Unknown syscall %d\n", syscall_number);
        /* we don't want to reply to an unknown syscall */

    }
    if (timer_stop) stop_timer();
    /* Free the saved reply cap */
    cspace_free_slot(cur_cspace, reply_cap);
}

void syscall_loop(seL4_CPtr ep) {

    while (1) {
        seL4_Word badge;
        seL4_Word label;
        seL4_MessageInfo_t message;

        message = seL4_Wait(ep, &badge);
        label = seL4_MessageInfo_get_label(message);
        if(badge & IRQ_EP_BADGE){
            /* Interrupt */
            if (badge & IRQ_BADGE_NETWORK) {
                network_irq();
            }
            if (badge &  IRQ_BADGE_CLOCK) {
                timer_interrupt();
                printf("current tick is %llu\n", time_stamp());
            }
        }else if(label == seL4_VMFault){
            /* Page fault */
            dprintf(0, "vm fault at 0x%08x, pc = 0x%08x, %s\n", seL4_GetMR(1),
                    seL4_GetMR(0),
                    seL4_GetMR(2) ? "Instruction Fault" : "Data fault");

            assert(!"Unable to handle vm faults");
        }else if(label == seL4_NoFault) {
            /* System call */
            handle_syscall(badge, seL4_MessageInfo_get_length(message) - 1);

        }else{
            printf("Rootserver got an unknown message\n");
        }
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

    dprintf(1,"-----------------------------------------\n\n");

    /* Print cpio data */
    dprintf(1,"Parsing cpio data:\n");
    dprintf(1,"--------------------------------------------------------\n");
    dprintf(1,"| index |        name      |  address   | size (bytes) |\n");
    dprintf(1,"|------------------------------------------------------|\n");
    for(i = 0;; i++) {
        unsigned long size;
        const char *name;
        void *data;

        data = cpio_get_entry(_cpio_archive, i, &name, &size);
        if(data != NULL){
            dprintf(1,"| %3d   | %16s | %p | %12d |\n", i, name, data, size);
        }else{
            break;
        }
    }
    dprintf(1,"--------------------------------------------------------\n");
}

void start_first_process(char* app_name, seL4_CPtr fault_ep) {
    int err;

    seL4_Word stack_addr;
    seL4_CPtr stack_cap;
    seL4_CPtr user_ep_cap;

    /* These required for setting up the TCB */
    seL4_UserContext context;

    /* These required for loading program sections */
    char* elf_base;
    unsigned long elf_size;

    /* Create a VSpace */
    tty_test_process.vroot_addr = ut_alloc(seL4_PageDirBits);
    conditional_panic(!tty_test_process.vroot_addr, 
                      "No memory for new Page Directory");
    err = cspace_ut_retype_addr(tty_test_process.vroot_addr,
                                seL4_ARM_PageDirectoryObject,
                                seL4_PageDirBits,
                                cur_cspace,
                                &tty_test_process.vroot);
    conditional_panic(err, "Failed to allocate page directory cap for client");

    /* Create a simple 1 level CSpace */
    tty_test_process.croot = cspace_create(1);
    assert(tty_test_process.croot != NULL);

    /* Create an IPC buffer */
    tty_test_process.ipc_buffer_addr = ut_alloc(seL4_PageBits);
    conditional_panic(!tty_test_process.ipc_buffer_addr, "No memory for ipc buffer");
    err =  cspace_ut_retype_addr(tty_test_process.ipc_buffer_addr,
                                 seL4_ARM_SmallPageObject,
                                 seL4_PageBits,
                                 cur_cspace,
                                 &tty_test_process.ipc_buffer_cap);
    conditional_panic(err, "Unable to allocate page for IPC buffer");

    /* Copy the fault endpoint to the user app to enable IPC */
    user_ep_cap = cspace_mint_cap(tty_test_process.croot,
                                  cur_cspace,
                                  fault_ep,
                                  seL4_AllRights, 
                                  seL4_CapData_Badge_new(TTY_EP_BADGE));
    /* should be the first slot in the space, hack I know */
    assert(user_ep_cap == 1);
    assert(user_ep_cap == USER_EP_CAP);

    /* Create a new TCB object */
    tty_test_process.tcb_addr = ut_alloc(seL4_TCBBits);
    conditional_panic(!tty_test_process.tcb_addr, "No memory for new TCB");
    err =  cspace_ut_retype_addr(tty_test_process.tcb_addr,
                                 seL4_TCBObject,
                                 seL4_TCBBits,
                                 cur_cspace,
                                 &tty_test_process.tcb_cap);
    conditional_panic(err, "Failed to create TCB");

    /* Configure the TCB */
    err = seL4_TCB_Configure(tty_test_process.tcb_cap, user_ep_cap, TTY_PRIORITY,
                             tty_test_process.croot->root_cnode, seL4_NilData,
                             tty_test_process.vroot, seL4_NilData, PROCESS_IPC_BUFFER,
                             tty_test_process.ipc_buffer_cap);
    conditional_panic(err, "Unable to configure new TCB");


    /* parse the cpio image */
    dprintf(1, "\nStarting \"%s\"...\n", app_name);
    elf_base = cpio_get_file(_cpio_archive, app_name, &elf_size);
    conditional_panic(!elf_base, "Unable to locate cpio header");

    /* load the elf image */
    err = elf_load(tty_test_process.vroot, elf_base);
    conditional_panic(err, "Failed to load elf image");


    /* Create a stack frame */
    stack_addr = ut_alloc(seL4_PageBits);
    conditional_panic(!stack_addr, "No memory for stack");
    err =  cspace_ut_retype_addr(stack_addr,
                                 seL4_ARM_SmallPageObject,
                                 seL4_PageBits,
                                 cur_cspace,
                                 &stack_cap);
    conditional_panic(err, "Unable to allocate page for stack");

    /* Map in the stack frame for the user app */
    err = map_page(stack_cap, tty_test_process.vroot,
                   PROCESS_STACK_TOP - (1 << seL4_PageBits),
                   seL4_AllRights, seL4_ARM_Default_VMAttributes);
    conditional_panic(err, "Unable to map stack IPC buffer for user app");

    /* Map in the IPC buffer for the thread */
    err = map_page(tty_test_process.ipc_buffer_cap, tty_test_process.vroot,
                   PROCESS_IPC_BUFFER,
                   seL4_AllRights, seL4_ARM_Default_VMAttributes);
    conditional_panic(err, "Unable to map IPC buffer for user app");

    /* Start the new process */
    memset(&context, 0, sizeof(context));
    context.pc = elf_getEntryPoint(elf_base);
    context.sp = PROCESS_STACK_TOP;
    seL4_TCB_WriteRegisters(tty_test_process.tcb_cap, 1, 0, 2, &context);
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

    /* Initialise the untyped sub system and reserve memory for DMA */
    err = ut_table_init(_boot_info);
    conditional_panic(err, "Failed to initialise Untyped Table\n");
    /* DMA uses a large amount of memory that will never be freed */
    dma_addr = ut_steal_mem(DMA_SIZE_BITS);
    conditional_panic(dma_addr == 0, "Failed to reserve DMA memory\n");

    /* find available memory */
    ut_find_memory(&low, &high);

    /* Initialise the untyped memory allocator */
    ut_allocator_init(low, high);

    /* Initialise the cspace manager */
    err = cspace_root_task_bootstrap(ut_alloc, ut_free, ut_translate,
                                     malloc, free);
    conditional_panic(err, "Failed to initialise the c space\n");

    /* Initialise DMA memory */
    err = dma_init(dma_addr, DMA_SIZE_BITS);
    conditional_panic(err, "Failed to intiialise DMA memory\n");

    /* Initialiase other system compenents here */

    _sos_ipc_init(ipc_ep, async_ep);
}

static inline seL4_CPtr badge_irq_ep(seL4_CPtr ep, seL4_Word badge) {
    seL4_CPtr badged_cap = cspace_mint_cap(cur_cspace, cur_cspace, ep, seL4_AllRights, seL4_CapData_Badge_new(badge | IRQ_EP_BADGE));
    conditional_panic(!badged_cap, "Failed to allocate badged cap");
    return badged_cap;
}

void* sync_new_ep(seL4_CPtr* ep, int badge) {

    seL4_Word aep_addr = ut_alloc(seL4_EndpointBits);
    conditional_panic(!aep_addr, "No memory for mutex async endpoint");
    int err = cspace_ut_retype_addr(aep_addr,
                                seL4_AsyncEndpointObject,
                                seL4_EndpointBits,
                                cur_cspace,
                                ep);
    conditional_panic(err, "Failed to allocate c-slot for Interrupt endpoint");

    ///* Bind the Async endpoint to our TCB */
    //err = seL4_TCB_BindAEP(seL4_CapInitThreadTCB, *ep);
    //conditional_panic(err, "Failed to bind ASync EP to TCB");
    *ep = cspace_mint_cap(cur_cspace, cur_cspace, *ep, seL4_AllRights, seL4_CapData_Badge_new(badge));
    return (void*)ep;
}

void sync_free_ep(void* ep){
    (void)ep;
}

static void print_time(uint32_t id, void *data) {
    (void) data;
    printf("%d expired at %llu\n", id, time_stamp());
}

static void stop(uint32_t id, void *data) {
    printf("timer stopped at %llu\n", time_stamp());
    timer_stop = true;
}

//uint32_t register_timer(uint64_t delay, void (*callback)(uint32_t id, void *data), void *data)
static void setup_timers(void) {
       register_timer(1000000, print_time, NULL);
       register_timer(5000000, print_time, NULL);
       register_timer(10000000, print_time, NULL);
//      register_timer(30000000, stop, NULL);
}

#define test_assert(tst)        \
    do {                        \
        if(!tst){               \
            printf("FAILED\n"); \
            assert(tst);        \
        }                       \
    }while(0)

static void test_mutex(void){
    sync_mutex_t m1, m2;
    printf("UNIT TEST | sync/mutex: ");
    m1 = sync_create_mutex();
    test_assert(m1);
    m2 = sync_create_mutex();
    test_assert(m2);
    /* Test simple operations */
    test_assert(sync_try_acquire(m1));
    test_assert(!sync_try_acquire(m1));
    sync_release(m1);
    test_assert(sync_try_acquire(m1));
    sync_release(m1);
    sync_acquire(m1);
    sync_release(m1);
    sync_acquire(m1);
    sync_release(m1);
    /* Test independance */
    test_assert(sync_try_acquire(m1));
    test_assert(sync_try_acquire(m2));
    sync_release(m1);
    sync_release(m2);
    /* Clean up */
    sync_destroy_mutex(m1);
    sync_destroy_mutex(m2);
    printf("PASSED\n");
}

/*
 * Main entry point - called by crt.
 */
int main(void) {

    dprintf(0, "\nSOS Starting...\n");

    _sos_init(&_sos_ipc_ep_cap, &_sos_interrupt_ep_cap);

    dprintf(0, "\ninit timer ...\n");
    /* Initialise and start the clock driver */
    timer_init(badge_irq_ep(_sos_interrupt_ep_cap, IRQ_BADGE_CLOCK));

    dprintf(0, "\nafter init timer\n");
    /* Initialise the network hardware */
    network_init(badge_irq_ep(_sos_interrupt_ep_cap, IRQ_BADGE_NETWORK));

    setup_timers();

    /* Start the user application */
//    start_first_process(TTY_NAME, _sos_ipc_ep_cap);

    test_mutex();
    /* Wait on synchronous endpoint for IPC */
    dprintf(0, "\nSOS entering syscall loop\n");
    syscall_loop(_sos_ipc_ep_cap);

    /* Not reached */
    return 0;
}
