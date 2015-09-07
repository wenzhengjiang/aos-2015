#include <errno.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>

#include <syscallno.h>
#include <sos.h>
#include <clock/clock.h>

#include "handler.h"
#include "addrspace.h"
#include "process.h"
#include "syscall.h"

#define MAX_SYSCALL_NO (100)
#define verbose 5
#include <log/debug.h>
#include <log/panic.h>

typedef int (*syscall_handler)(seL4_CPtr reply_cap);

static syscall_handler handlers[MAX_SYSCALL_NO];
static sos_proc_t * cur_proc;

static cont_t empty_cont;

static char path[MAX_FILE_PATH_LENGTH];

int sos_vm_fault(seL4_Word read_fault, seL4_Word faultaddr) {
    sos_addrspace_t *as = proc_as(current_process());
    if (as == NULL) {
        return EFAULT;
    }

    sos_region_t* reg = as_vaddr_region(as, faultaddr);
    if (!reg) {
        return EFAULT;
    }
    if (read_fault && !(reg->rights & seL4_CanRead)) {
        return EACCES;
    }
    if (!read_fault && !(reg->rights & seL4_CanWrite)) {
        return EACCES;
    }
    int err = as_create_page(as, faultaddr, reg->rights);
    if (err) {
        return err;
    }
    return 0;
}

inline static void send_back(seL4_CPtr reply_cap, seL4_MessageInfo_t reply) {
    seL4_SetTag(reply);
    seL4_Send(reply_cap, reply);
    cspace_free_slot(cur_cspace, reply_cap);
    cur_proc->cont = empty_cont;
}

static void sys_notify_client(uint32_t id, void *data) {
    seL4_CPtr reply_cap = (seL4_CPtr)data;
    seL4_MessageInfo_t reply = seL4_MessageInfo_new(seL4_NoFault,0,0,0);
    send_back(reply_cap, reply);
}

static int brk_handler (seL4_CPtr reply_cap) {
    dprintf(4, "SYS BRK\n");
    sos_addrspace_t* as = proc_as(current_process());
    assert(as);
    client_vaddr brk = sos_brk(as, seL4_GetMR(1));
    seL4_MessageInfo_t reply = seL4_MessageInfo_new(seL4_NoFault, 0, 0, 1);
    seL4_SetMR(0, brk);
    send_back(reply_cap, reply);

    return 0;
}

static int usleep_handler (seL4_CPtr reply_cap) {
    dprintf(4, "SYS SLEEP\n");
    uint64_t delay = 1000ULL * seL4_GetMR(1);
    if(!register_timer(delay, sys_notify_client, (int*)reply_cap)) {
        seL4_MessageInfo_t reply = seL4_MessageInfo_new(seL4_UserException,0,0,0);
        send_back(reply_cap, reply);
        return 1;
    }
    return 0;
}

static int timestamp_handler (seL4_CPtr reply_cap) {
    dprintf(4, "SYS TIME %d\n", reply_cap);
    uint64_t tick = time_stamp();
    seL4_MessageInfo_t reply = seL4_MessageInfo_new(seL4_NoFault,0,0,2);
    seL4_SetMR(0, tick & 0xffffffff);
    seL4_SetMR(1, tick>>32);
    send_back(reply_cap, reply);
    return 0;
}

static int open_handler (seL4_CPtr reply_cap) {
    dprintf(4, "SYS OPEN: %d\n", reply_cap);
    fmode_t mode = seL4_GetMR(1);
    memset(path, 0, sizeof(path));
    ipc_read(OPEN_MESSAGE_START, path);
    cur_proc->cont.reply_cap = reply_cap;
    return sos__sys_open(path, mode);
}

static int read_handler (seL4_CPtr reply_cap) {
    dprintf(4, "SYS READ: %d\n", reply_cap);
    int file = (int) seL4_GetMR(1);
    client_vaddr buf = seL4_GetMR(2);
    size_t nbyte = (size_t) seL4_GetMR(3);
    cur_proc->cont.reply_cap = reply_cap;

    return sos__sys_read(file, buf, nbyte);
}

static int write_handler (seL4_CPtr reply_cap) {
    dprintf(4, "SYS WRITE: %d\n", reply_cap);
    int file = (int) seL4_GetMR(1);
    client_vaddr buf = seL4_GetMR(2);
    size_t nbyte = (size_t) seL4_GetMR(3);
    cur_proc->cont.reply_cap = reply_cap;

    return sos__sys_write(file, buf, nbyte);
}

static int getdirent_handler (seL4_CPtr reply_cap) {
    int pos = seL4_GetMR(1);
    dprintf(4, "SYS GETDIRENT %u\n", pos);
    client_vaddr name = (client_vaddr)seL4_GetMR(2);
    size_t nbyte = seL4_GetMR(3);
    cur_proc->cont.reply_cap = reply_cap;

    return sos__sys_getdirent(pos, name, nbyte);
}

static int stat_handler (seL4_CPtr reply_cap) {
    client_vaddr buf = (client_vaddr) seL4_GetMR(1);
    dprintf(4, "SYS STAT\n");
    memset(path, 0, sizeof(path));
    ipc_read(STAT_MESSAGE_START, path);
    cur_proc->cont.reply_cap = reply_cap;

    return sos__sys_stat(path, buf);
}

static int close_handler (seL4_CPtr reply_cap) {
    int res;
    seL4_MessageInfo_t reply;
    int fd = (int)seL4_GetMR(1);
    dprintf(4, "SYS CLOSE\n");
    res = sos__sys_close(fd);
    if (res != 0) {
        reply = seL4_MessageInfo_new(seL4_UserException,0,0,1);
    } else {
        reply = seL4_MessageInfo_new(seL4_NoFault,0,0,1);
    }
    dprintf(4, "[CLOSE] Replying with res: %d\n", res);
    seL4_SetMR(0, (seL4_Word)res);
    send_back(reply_cap, reply);
    return 0;
}

void register_handlers(void) {
    assert(handlers[SOS_SYSCALL_BRK] == NULL);
    handlers[SOS_SYSCALL_BRK] = brk_handler;

    assert(handlers[SOS_SYSCALL_USLEEP] == NULL);
    handlers[SOS_SYSCALL_USLEEP]  = usleep_handler;

    assert(handlers[SOS_SYSCALL_TIMESTAMP] == NULL);
    handlers[SOS_SYSCALL_TIMESTAMP] = timestamp_handler;

    assert(handlers[SOS_SYSCALL_OPEN] == NULL);
    handlers[SOS_SYSCALL_OPEN] = open_handler;

    assert(handlers[SOS_SYSCALL_READ] == NULL);
    handlers[SOS_SYSCALL_READ] = read_handler;

    assert(handlers[SOS_SYSCALL_WRITE] == NULL);
    handlers[SOS_SYSCALL_WRITE] = write_handler;

    assert(handlers[SOS_SYSCALL_GETDIRENT] == NULL);
    handlers[SOS_SYSCALL_GETDIRENT] = getdirent_handler;

    assert(handlers[SOS_SYSCALL_STAT] == NULL);
    handlers[SOS_SYSCALL_STAT] = stat_handler;

    assert(handlers[SOS_SYSCALL_CLOSE] == NULL);
    handlers[SOS_SYSCALL_CLOSE] = close_handler;
}

void handle_syscall(seL4_Word badge, int num_args) {
    cur_proc = current_process();
    seL4_Word syscall_number = seL4_GetMR(0);
    /* Save the caller */
    seL4_CPtr reply_cap = cspace_save_reply_cap(cur_cspace);
    assert(reply_cap != CSPACE_NULL);
    seL4_MessageInfo_t reply;

    assert(syscall_number > 0 && syscall_number < MAX_SYSCALL_NO);

    if (handlers[syscall_number]) {

        int ret = handlers[syscall_number](reply_cap);
        assert(ret >= 0);
        if (ret > 0) {
            reply = seL4_MessageInfo_new(seL4_UserException, 0, 0, 1);
            seL4_SetMR(0, (seL4_Word)ret);
            send_back(reply_cap, reply);
        }
    } else {
        printf("Unknown syscall %d\n", syscall_number);
    }
}
