#include <errno.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>

#include <syscallno.h>
#include <sos.h>
#include <clock/clock.h>

#include "handler.h"
#include "addrspace.h"
#include "page_replacement.h"
#include "process.h"
#include "syscall.h"

#define HANDLER_TYPES  (2)

#define HANDLER_SETUP  (0)
#define HANDLER_EXEC   (1)

#define MAX_SYSCALL_NO (100)
#define verbose 0
#include <log/debug.h>
#include <log/panic.h>

typedef int (*syscall_handler)(void);

static syscall_handler handlers[MAX_SYSCALL_NO][HANDLER_TYPES];
static sos_proc_t * cur_proc;

static cont_t empty_cont;

static bool is_read_fault(seL4_Word faulttype) {
    return (faulttype & (1 << 11)) == 0;
}

int sos_vm_fault(seL4_Word faulttype, seL4_Word faultaddr) {
    sos_proc_t *proc = current_process();
    sos_addrspace_t *as = proc_as(proc);
    if (as == NULL) {
        return EFAULT;
    }

    sos_region_t* reg = as_vaddr_region(as, faultaddr);
    if (!reg) {
        printf("addr %x is not in the reg\n", faultaddr);
        return EFAULT;
    }
    if (is_read_fault(faulttype) && !(reg->rights & seL4_CanRead)) {
        return EACCES;
    }
    if (!is_read_fault(faulttype) && !(reg->rights & seL4_CanWrite)) {
        return EACCES;
    }
    dprintf(-1, "sos_vm_fault %08x\n", faultaddr);
    if (as_page_exists(as, faultaddr)) {
        dprintf(4, "page exists\n");
        if (swap_is_page_swapped(as, faultaddr)) { // page is in disk
            printf("page is in disk\n");
            swap_replace_page(as, faultaddr);
        } else if (is_referenced(as, faultaddr)) {
            // Page exists, referenced bit is set (so it must be mapped w/
            // correct permissions), yet it faulted?!
            assert(!"This shouldn't happen");
        } else {
            as_reference_page(as, faultaddr, reg->rights);
        }
    } else {
        dprintf(4, "create new page\n");
        process_create_page(faultaddr, reg->rights);
    }
    return 0;
}

inline static void send_back(seL4_MessageInfo_t reply) {
    seL4_CPtr reply_cap = current_process()->cont.reply_cap;
    seL4_SetTag(reply);
    seL4_Send(reply_cap, reply);
    cspace_free_slot(cur_cspace, reply_cap);
    cur_proc->cont = empty_cont;
}

static void sys_notify_client(uint32_t id, void *data) {
    seL4_MessageInfo_t reply = seL4_MessageInfo_new(seL4_NoFault,0,0,0);
    send_back(reply);
}

static int brk_handler (void) {
    dprintf(4, "SYS BRK\n");
    sos_addrspace_t* as = proc_as(current_process());
    assert(as);
    client_vaddr brk = sos_brk(as, seL4_GetMR(1));
    seL4_MessageInfo_t reply = seL4_MessageInfo_new(seL4_NoFault, 0, 0, 1);
    seL4_SetMR(0, brk);
    send_back(reply);

    return 0;
}

static int usleep_handler (void) {
    dprintf(4, "SYS SLEEP\n");
    uint64_t delay = 1000ULL * seL4_GetMR(1);
    if(!register_timer(delay, sys_notify_client, (int*)current_process()->cont.reply_cap)) {
        seL4_MessageInfo_t reply = seL4_MessageInfo_new(seL4_UserException,0,0,0);
        send_back(reply);
        return 1;
    }
    return 0;
}

static int timestamp_handler (void) {
    dprintf(4, "SYS TIME\n");
    uint64_t tick = time_stamp();
    seL4_MessageInfo_t reply = seL4_MessageInfo_new(seL4_NoFault,0,0,2);
    seL4_SetMR(0, tick & 0xffffffff);
    seL4_SetMR(1, tick>>32);
    send_back(reply);
    return 0;
}

static int open_setup (void) {
    dprintf(4, "SYS OPEN\n");
    current_process()->cont.file_mode = (fmode_t)seL4_GetMR(1);
    memset(current_process()->cont.path, 0,
           sizeof(current_process()->cont.path));
    ipc_read(OPEN_MESSAGE_START, current_process()->cont.path);
    return 0;
}

static int read_setup (void) {
    dprintf(4, "SYS READ\n");
    current_process()->cont.fd = (int)seL4_GetMR(1);
    current_process()->cont.client_addr = seL4_GetMR(2);
    current_process()->cont.length_arg = (size_t)seL4_GetMR(3);
    return 0;
}

static int write_setup (void) {
    dprintf(4, "SYS WRITE\n");
    current_process()->cont.fd = (int)seL4_GetMR(1);
    current_process()->cont.client_addr = seL4_GetMR(2);
    current_process()->cont.length_arg = (size_t)seL4_GetMR(3);
    return 0;
}

static int getdirent_setup (void) {
    dprintf(4, "SYS GETDIRENT\n");
    current_process()->cont.position_arg = (int)seL4_GetMR(1);
    current_process()->cont.client_addr = (client_vaddr)seL4_GetMR(2);
    current_process()->cont.length_arg = seL4_GetMR(3);
    return 0;
}

static int stat_setup (void) {
    current_process()->cont.client_addr = (client_vaddr)seL4_GetMR(1);
    dprintf(4, "SYS STAT\n");
    memset(current_process()->cont.path, 0,
           sizeof(current_process()->cont.path));
    ipc_read(STAT_MESSAGE_START, current_process()->cont.path);
    return 0;
}

static int close_handler (void) {
    dprintf(4, "SYS CLOSE\n");
    int res;
    seL4_MessageInfo_t reply;
    current_process()->cont.fd = (int)seL4_GetMR(1);
    res = sos__sys_close();
    if (res != 0) {
        reply = seL4_MessageInfo_new(seL4_UserException,0,0,1);
    } else {
        reply = seL4_MessageInfo_new(seL4_NoFault,0,0,1);
    }
    dprintf(4, "[CLOSE] Replying with res: %d\n", res);
    seL4_SetMR(0, (seL4_Word)res);
    send_back(reply);
    return 0;
}

void register_handlers(void) {
    handlers[SOS_SYSCALL_BRK][HANDLER_SETUP] = NULL;
    handlers[SOS_SYSCALL_BRK][HANDLER_EXEC] = brk_handler;

    handlers[SOS_SYSCALL_USLEEP][HANDLER_SETUP] = NULL;
    handlers[SOS_SYSCALL_USLEEP][HANDLER_EXEC] = usleep_handler;

    handlers[SOS_SYSCALL_TIMESTAMP][HANDLER_SETUP] = NULL;
    handlers[SOS_SYSCALL_TIMESTAMP][HANDLER_EXEC] = timestamp_handler;

    handlers[SOS_SYSCALL_OPEN][HANDLER_SETUP] = open_setup;
    handlers[SOS_SYSCALL_OPEN][HANDLER_EXEC] = sos__sys_open;

    handlers[SOS_SYSCALL_READ][HANDLER_SETUP] = read_setup;
    handlers[SOS_SYSCALL_READ][HANDLER_EXEC] = sos__sys_read;

    handlers[SOS_SYSCALL_WRITE][HANDLER_SETUP] = write_setup;
    handlers[SOS_SYSCALL_WRITE][HANDLER_EXEC] = sos__sys_write;

    handlers[SOS_SYSCALL_GETDIRENT][HANDLER_SETUP] = getdirent_setup;
    handlers[SOS_SYSCALL_GETDIRENT][HANDLER_EXEC] = sos__sys_getdirent;

    handlers[SOS_SYSCALL_STAT][HANDLER_SETUP] = stat_setup;
    handlers[SOS_SYSCALL_STAT][HANDLER_EXEC] = sos__sys_stat;

    handlers[SOS_SYSCALL_CLOSE][HANDLER_SETUP] = NULL;
    handlers[SOS_SYSCALL_CLOSE][HANDLER_EXEC] = close_handler;
}

void handle_syscall(seL4_Word syscall_number) {
    /* Save the caller */
    seL4_MessageInfo_t reply;
    int ret;
    assert(syscall_number > 0 && syscall_number < MAX_SYSCALL_NO);

    if (handlers[syscall_number][HANDLER_SETUP] != NULL &&
        !current_process()->cont.handler_initiated) {
        ret = handlers[syscall_number][HANDLER_SETUP]();
        assert(ret >= 0);
        current_process()->cont.handler_initiated = true;
        if (ret > 0) {
            reply = seL4_MessageInfo_new(seL4_UserException, 0, 0, 1);
            seL4_SetMR(0, (seL4_Word)ret);
            send_back(reply);
        }
    }
    if (handlers[syscall_number][HANDLER_EXEC]) {
        ret = handlers[syscall_number][HANDLER_EXEC]();
        assert(ret >= 0);
        if (ret > 0) {
            reply = seL4_MessageInfo_new(seL4_UserException, 0, 0, 1);
            seL4_SetMR(0, (seL4_Word)ret);
            send_back(reply);
        }
    } else {
        printf("Unknown syscall %d\n", syscall_number);
    }
}
