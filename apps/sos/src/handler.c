/**
 * @file handler.c
 * @brief Transfer IPC syscall request to sos internal syscall
 */

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
#include "elf.h"

#define HANDLER_TYPES  (2)
#define PAGE_ALIGN(a) (a & 0xfffff000)

#define HANDLER_SETUP  (0)
#define HANDLER_EXEC   (1)

#define MAX_SYSCALL_NO (100)
#define verbose 5
#include <log/debug.h>
#include <log/panic.h>

typedef int (*syscall_handler)(void);

/*Handler functions to setup and execute syscalls */
static syscall_handler handlers[MAX_SYSCALL_NO][HANDLER_TYPES];

static bool is_read_fault(seL4_Word faulttype) {
    return (faulttype & (1 << 11)) == 0;
}

/**
 * @brief VM fault handler function
 */
int sos_vm_fault(seL4_Word faulttype, seL4_Word faultaddr) {
    sos_proc_t *proc = current_process();
    sos_addrspace_t *as = proc_as(proc);
    if (as == NULL) {
        return EFAULT;
    }

    sos_region_t* reg = as_vaddr_region(as, faultaddr);

    if (!reg) {
        ERR("[VMF] Addr %x is not in the reg\n", faultaddr);
        return EFAULT;
    }
    if (is_read_fault(faulttype) && !(reg->rights & seL4_CanRead)) {
        ERR("[VMF] Cannot read address: %x\n", faultaddr);
        return EACCES;
    }
    if (!is_read_fault(faulttype) && !(reg->rights & seL4_CanWrite)) {
        ERR("[VMF] Cannot write to address: %x\n", faultaddr);
        return EACCES;
    }

    dprintf(-1, "sos_vm_fault %08x\n", faultaddr);
    /* Fault on an existing page and 
     * we're not re-entering this handler in the middle of loading page from elf file*/
    if (as_page_exists(as, faultaddr) && !proc->cont.binary_nfs_read) {
        if (swap_is_page_swapped(as, faultaddr)) { // fault on a page in disk 
            swap_in_page(faultaddr); 
            as_reference_page(current_process()->vspace, faultaddr, reg->rights);
            current_process()->cont.page_eviction_process = NULL;
        } else if (!is_referenced(as, faultaddr)) { // fault on a page whose reference bit is 0
            as_reference_page(as, faultaddr, reg->rights);
        } else {
            assert(!"This shouldn't happen");
        }
    } else { /* Fault on an new page */
        if (!proc->cont.create_page_done) {
            if (process_create_page(faultaddr, reg->rights)) 
                return ENOMEM;
            proc->cont.create_page_done = true;
        }
        dprintf(4, "[VMF] page doesn't exist\n");
        pte_t *pt = as_lookup_pte(as, faultaddr);
        if (pt == NULL) {
            return EFAULT;
        }
        seL4_Word aligned_addr = PAGE_ALIGN(faultaddr);
        dprintf(4, "[VMF] reg->elf:%x\n", reg->elf_addr);
        if (reg->elf_addr != -1) { // If the new page is code segment, it needs to be loaded into vspace 
            dprintf(4, "[VMF] %08x -- %08x, %x", reg->start, reg->end, reg->rights);
            proc->cont.fd = BINARY_READ_FD;
            dprintf(4, "[VMF] LOADING INTO VSPACE\n");
            if (aligned_addr < reg->start) {
                aligned_addr = reg->start;
            }
            int err = load_page_into_vspace(proc,
                                        reg->elf_addr,
                                        faultaddr);
            if (err) {
                ERR("FAILED TO LOAD PAGE FOR PROC\n");
                process_delete(proc);
            }
        }
    }
    return 0;
}

static int brk_setup(void) {
    dprintf(4, "SYS BRK\n");
    current_process()->cont.brk = seL4_GetMR(1);
    return 0;
}

static int usleep_setup(void) {
    dprintf(4, "SYS SLEEP\n");
    current_process()->cont.delay = 1000ULL * seL4_GetMR(1);
    return 0;
}

static int open_setup (void) {
    current_process()->cont.file_mode = (fmode_t)seL4_GetMR(1);
    memset(current_process()->cont.path, 0, MAX_FILE_PATH_LENGTH);
    ipc_read_str(OPEN_MESSAGE_START, current_process()->cont.path);
    
    dprintf(4, "SYS OPEN %s\n", current_process()->cont.path);
    dprintf(4, "ipc %x %x %x %x\n", seL4_GetMR(2), seL4_GetMR(3), seL4_GetMR(4));
    io_device_t *dev = device_handler_str(current_process()->cont.path);
    int fd = fd_create(current_process()->fd_table, NULL, dev,
                       current_process()->cont.file_mode);
    if (fd < 0) 
        return ENOMEM;
    current_process()->cont.fd = fd;
    return 0;
}

static int read_setup (void) {
    dprintf(4, "SYS READ\n");
    client_vaddr buf = seL4_GetMR(2);
    size_t nbyte = (size_t)seL4_GetMR(3);
    if (buf == 0) {
        return EINVAL;
    }
    current_process()->cont.fd = (int)seL4_GetMR(1);
    current_process()->cont.client_addr = buf;
    current_process()->cont.length_arg = nbyte;
    current_process()->cont.iov = cbuf_to_iov(buf, nbyte, WRITE);
    if (current_process()->cont.iov == NULL) {
        return ENOMEM;
    }
    return 0;
}

static int write_setup (void) {
    dprintf(4, "SYS WRITE\n");
    int fd = (int)seL4_GetMR(1);
    client_vaddr buf = seL4_GetMR(2);
    size_t nbyte = (size_t)seL4_GetMR(3);
    if (fd < 0 || buf == 0) {
        return EINVAL;
    }

    current_process()->cont.fd = fd;
    current_process()->cont.client_addr = buf;
    current_process()->cont.length_arg = nbyte;
    current_process()->cont.iov = cbuf_to_iov(buf, nbyte, READ);
    if (current_process()->cont.iov == NULL) {
        return EINVAL;
    }
    return 0;
}

static int getdirent_setup (void) {
    dprintf(4, "SYS GETDIRENT\n");
    size_t nbyte = (size_t)seL4_GetMR(2);
    unsigned int pos = (int)seL4_GetMR(1);
    if (nbyte == 0 || pos == 0) return EINVAL;
    current_process()->cont.position_arg = pos;
    current_process()->cont.length_arg = nbyte;
    return 0;
}

static int waitpid_setup(void) {
    dprintf(4, "SYS WAITPID\n");
    pid_t pid = (int)seL4_GetMR(1);
    if (pid <= 0) return EINVAL;
    current_process()->cont.pid = pid;
    return 0;
}

static int proc_delete_setup(void) {
    dprintf(4, "SYS PROC DELETE\n");
    pid_t pid = seL4_GetMR(1);
    if (pid <= 0) return EINVAL;
    current_process()->cont.pid = pid;
    return 0;
}

static int proc_status_setup(void) {
    dprintf(4, "SYS PROC STATUS\n");
    client_vaddr buf = seL4_GetMR(1);
    size_t maxn = (size_t)seL4_GetMR(2);

    char *stat_buf = malloc(maxn * sizeof(sos_process_t));
    if (stat_buf == NULL || buf == 0) return ENOMEM;
    int bytes = get_all_proc_stat(stat_buf, maxn);
    assert(bytes <= maxn * sizeof(sos_process_t));

    current_process()->cont.proc_stat_buf = stat_buf;
    current_process()->cont.proc_stat_n = bytes / sizeof(sos_process_t);
    current_process()->cont.iov = cbuf_to_iov(buf, bytes, WRITE);
    if (current_process()->cont.iov == NULL) {
        free(stat_buf);
        return EINVAL;
    }
    return 0;
}

static int stat_setup (void) {
    current_process()->cont.client_addr = (client_vaddr)seL4_GetMR(1);
    dprintf(4, "SYS STAT\n");
    memset(current_process()->cont.path, 0, MAX_FILE_PATH_LENGTH);
    ipc_read_str(STAT_MESSAGE_START, current_process()->cont.path);
    return 0;
}

static int close_setup(void) {
    dprintf(4, "SYS CLOSE\n");
    int fd = (int)seL4_GetMR(1);
    current_process()->cont.fd = fd ;
    return 0;
}

static int proc_create_setup(void) {
    dprintf(4, "SYS PROC_CREATE\n");
    memset(current_process()->cont.path, 0, MAX_FILE_PATH_LENGTH);
    ipc_read_str(PROC_CREATE_MESSAGE_START, current_process()->cont.path);
    return 0;
}

void register_handlers(void) {
    handlers[SOS_SYSCALL_BRK][HANDLER_SETUP] = brk_setup;
    handlers[SOS_SYSCALL_BRK][HANDLER_EXEC] = sos__sys_brk;

    handlers[SOS_SYSCALL_USLEEP][HANDLER_SETUP] = usleep_setup;
    handlers[SOS_SYSCALL_USLEEP][HANDLER_EXEC] = sos__sys_usleep;

    handlers[SOS_SYSCALL_TIMESTAMP][HANDLER_SETUP] = NULL;
    handlers[SOS_SYSCALL_TIMESTAMP][HANDLER_EXEC] = sos__sys_timestamp;

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

    handlers[SOS_SYSCALL_CLOSE][HANDLER_SETUP] = close_setup;
    handlers[SOS_SYSCALL_CLOSE][HANDLER_EXEC] = sos__sys_close;

    handlers[SOS_SYSCALL_PROC_CREATE][HANDLER_SETUP] = proc_create_setup;
    handlers[SOS_SYSCALL_PROC_CREATE][HANDLER_EXEC] =  sos__sys_proc_create;

    handlers[SOS_SYSCALL_GETPID][HANDLER_SETUP] = NULL;
    handlers[SOS_SYSCALL_GETPID][HANDLER_EXEC] =  sos__sys_getpid;

    handlers[SOS_SYSCALL_WAITPID][HANDLER_SETUP] = waitpid_setup;
    handlers[SOS_SYSCALL_WAITPID][HANDLER_EXEC] =  sos__sys_waitpid;

    handlers[SOS_SYSCALL_PROC_DELETE][HANDLER_SETUP] = proc_delete_setup;
    handlers[SOS_SYSCALL_PROC_DELETE][HANDLER_EXEC] =  sos__sys_proc_delete;

    handlers[SOS_SYSCALL_PROC_STATUS][HANDLER_SETUP] = proc_status_setup;
    handlers[SOS_SYSCALL_PROC_STATUS][HANDLER_EXEC] =  sos__sys_proc_status;
}

void handle_syscall(seL4_Word syscall_number) {
    /* Save the caller */
    int err;
    dprintf(4, "Handling syscall number %d\n", syscall_number);
    assert(syscall_number > 0 && syscall_number < MAX_SYSCALL_NO);
    if (handlers[syscall_number][HANDLER_SETUP] != NULL &&
        !current_process()->cont.handler_initiated) {
        err = handlers[syscall_number][HANDLER_SETUP]();
        current_process()->cont.handler_initiated = true;
        if (err > 0) {
            syscall_end_continuation(current_process(), 0, false);
            return ;
        }
    }
    if (handlers[syscall_number][HANDLER_EXEC]) {
        err = handlers[syscall_number][HANDLER_EXEC]();
        if (err > 0) {
            syscall_end_continuation(current_process(), 0, false);
            return ;
        }
    } else {
        printf("Unknown syscall %d\n", syscall_number);
    }
}
