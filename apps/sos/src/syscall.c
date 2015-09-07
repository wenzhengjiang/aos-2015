#include <sel4/sel4.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>
#include <string.h>
#include <errno.h>
#include <serial/serial.h>
#include <cspace/cspace.h>
#include <limits.h>
#include "serial.h"
#include "frametable.h"
#include "process.h"
#include "file.h"
#include "addrspace.h"
#include "syscall.h"
#include <assert.h>
#include <sos.h>
#include <syscallno.h>
#include <clock/clock.h>

#define verbose 0
#include <log/debug.h>
#include <log/panic.h>

#define PRINT_MESSAGE_START (2)

extern io_device_t serial_io;
extern io_device_t nfs_io;

typedef enum iop_direction {READ, WRITE, NONE} iop_direction_t;

static inline unsigned CONST umin(unsigned a, unsigned b)
{
    return (a < b) ? a : b;
}
timestamp_t start_time, end_time;

const int pkg_size = 1284;
#define PKGS(n) ((n+pkg_size-1)/pkg_size)
void syscall_end_continuation(sos_proc_t *proc, int retval, bool success) {
    iovec_t *iov;
    seL4_MessageInfo_t reply;
    dprintf(4, "ENDING SYSCALL\n", retval);
    dprintf(4, "[SYSEND] Returning %d\n", retval);
    if (success) {
        reply = seL4_MessageInfo_new(seL4_NoFault, 0, 0, 1);
    } else {
        reply = seL4_MessageInfo_new(seL4_UserException, 0, 0, 1);
    }
    seL4_SetMR(0, retval);
    seL4_SetTag(reply);
    assert(proc->cont.reply_cap != seL4_CapNull);
    assert(proc->cont.reply_cap != seL4_CapNull);
    timestamp_t  elapsed = time_stamp() - start_time;
    dprintf(0, "%d %llu us, %0.2lf/byte, %0.2lf/pkg\n", retval, elapsed, (double)elapsed/retval, (double)elapsed/PKGS(retval));
    dprintf(0, " %llu\n", end_time-start_time);
    seL4_Send(proc->cont.reply_cap, reply);
    iov = proc->cont.iov;
    while(iov) {
        if (iov) {
            proc->cont.iov = iov->next;
        }
        free(iov);
        iov = proc->cont.iov;
    }
    memset(&proc->cont, 0, sizeof(cont_t));
    dprintf(4, "SYSCALL ENDED\n", retval);
}

static sos_vaddr
check_page(sos_addrspace_t *as, client_vaddr buf, iop_direction_t dir) {
    sos_region_t* reg = as_vaddr_region(as, buf);
    if (!reg) {
        return 0;
    }
    // Ensure client process has correct permissions to the page
    if (!(reg->rights & seL4_CanWrite) && dir == WRITE) {
        return 0;
    } else if (!(reg->rights & seL4_CanRead) && dir == READ) {
        return 0;
    }

    // Ensure client process has the page mapped
    sos_vaddr saddr = as_lookup_sos_vaddr(as, buf);
    if (saddr == 0) {
        int err = as_create_page(as, buf, reg->rights);
        if (err) return 0;
    }
    saddr = as_lookup_sos_vaddr(as, buf);

    return saddr;
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

void ipc_read(int start, char *buf) {
    assert(buf && start > 0);
    int k = 0, i ;
    for (i = start; i < seL4_MsgMaxLength; i++) {
       int len = unpack_word(buf+k, seL4_GetMR(i)); 
       k += len;
       if (len < sizeof(seL4_Word)) break;
    }
    if (k < MAX_FILE_PATH_LENGTH)
        buf[k] = 0;
}

static iovec_t* iov_create(size_t start, size_t sz, iovec_t *iohead, iovec_t *iotail) {
    iovec_t *ionew = malloc(sizeof(iovec_t));
    if (ionew == NULL) {
        iov_free(iohead);
        return NULL;
    }
    ionew->start = start;
    ionew->sz = sz;
    ionew->next = NULL;

    if (iohead == NULL) {
        return ionew;
    } else {
        iotail->next = ionew;
        return iohead;
    }
}

static iovec_t *cbuf_to_iov(client_vaddr buf, size_t nbyte, iop_direction_t dir) {
    sos_vaddr saddr;
    size_t remaining = nbyte;
    iovec_t *iohead = NULL;
    iovec_t *iotail = NULL;
    sos_addrspace_t* as = current_as();
    if (remaining == 0) {
        saddr = check_page(as, buf, dir);
        if (saddr == 0) {
            ERR("Client page lookup %x failed\n", buf);
            return NULL;
        }
        iohead = iov_create(saddr, 0, NULL, NULL);
        return iohead;
    }
    dprintf(1, "cbuf_to_iov: %d bytes\n", nbyte);
    while(remaining) {
        size_t offset = ((unsigned)buf % PAGE_SIZE);
        size_t buf_delta = umin((PAGE_SIZE - offset), remaining);
        saddr = check_page(as, buf, dir);
        if (saddr == 0) {
            ERR("Client page lookup %x failed\n", buf);
            return NULL;
        }
        dprintf(1, "cbuf_to_iov: delta=%d\n", buf_delta);
        iohead = iov_create(saddr, buf_delta, iohead, iotail);
        if (iotail == NULL) iotail = iohead;
        else iotail = iotail->next;

        if (iohead == NULL) {
            ERR("Insufficient memory to create new iovec\n");
            return NULL;
        }
        assert(iotail);
        remaining -= buf_delta;
        buf += buf_delta;
    }
    int cnt = 0;
    for (iovec_t* iov = iohead; iov; iov = iov->next) {
       cnt += iov->sz; 
    }
    dprintf(1, "cbuf_to_iov: iov=%d, nbyte=%d\n", cnt, nbyte);
    assert(cnt == nbyte);
    return iohead;
}

static io_device_t* device_handler_str(const char* filename) {
    if (strcmp(filename, "console") == 0) {
        return &serial_io;
    } else {
        return &nfs_io;
    }
}

static io_device_t* device_handler_fd(int fd) {
    sos_proc_t *curproc = current_process();
    assert(curproc);
    assert(curproc->fd_table);
    return curproc->fd_table[fd]->io;
}

static int translate_fcntl_mode(int mode) {
    switch(mode) {
    case 2:
        return 6;
    case 1:
        return 2;
    case 0:
        return 4;
    default:
        return 0;
    }
}

int sos__sys_open(const char *path, int mode) {
    io_device_t *dev = device_handler_str(path);
    int fmode = translate_fcntl_mode(mode);
    assert(dev);
    return dev->open(path, fmode);
}

int sos__sys_read(int file, client_vaddr buf, size_t nbyte){
    of_entry_t *of = fd_lookup(current_process(), file);
    if (of == NULL) {
        return 1;
    }
    if (!(of->mode & FM_READ)) {
        return EPERM;
    }
    io_device_t *dev = device_handler_fd(file);
    iovec_t *iov = cbuf_to_iov(buf, nbyte, WRITE);
    if (iov == NULL) {
        // TODO: Kill bad client
        assert(!"illegal buf addr");
        return EINVAL;
    }
    assert(dev);
    return dev->read(iov, file, nbyte);
}

int sos__sys_write(int file, client_vaddr buf, size_t nbyte) {
    of_entry_t *of = fd_lookup(current_process(), file);
    if (of == NULL) {
        return 1;
    }
    if (!(of->mode & FM_WRITE)) {
        printf("Check of mode %d failed\n", of->mode);
        return EPERM;
    }
    io_device_t *dev = device_handler_fd(file);
    iovec_t *iov = cbuf_to_iov(buf, nbyte, READ);
    if (iov == NULL) {
        // TODO: Kill bad client
        assert(!"illegal buf addr");
        return EINVAL;
    }
    assert(dev);
    dprintf(0, "write nbytes %u ", nbyte);
    start_time = time_stamp();
    return dev->write(iov, file, nbyte);
}

int sos__sys_stat(char *path, client_vaddr buf) {
    iovec_t *iov = cbuf_to_iov(buf, sizeof(sos_stat_t), WRITE);
    if (iov == NULL) {
        // TODO: Kill bad client
        assert(!"illegal buf addr");
        return EINVAL;
    }

    return nfs_io.stat(path, iov);
}

int sos__sys_getdirent(int pos, client_vaddr name, size_t nbyte) {
    iovec_t *iov = cbuf_to_iov(name, nbyte, WRITE);
    if (iov == NULL) {
        // TODO: Kill bad client
        assert(!"illegal buf addr");
        return EINVAL;
    }
    return nfs_io.getdirent(pos, iov);
}

/**
 * Close an open file.
 * Should not block the caller.
 */
int sos__sys_close(int file) {
    if (fd_lookup(current_process(), file) == NULL) {
        return EINVAL;
    }
    int res;
    of_entry_t* of = fd_lookup(current_process(), file);
    io_device_t *io = of->io;
    if (io == NULL) {
        res = fd_free(current_process(), file);
    } else if (io->close == NULL) {
        res = fd_free(current_process(), file);
    } else {
        res = io->close(file);
    }
    return res;
}
