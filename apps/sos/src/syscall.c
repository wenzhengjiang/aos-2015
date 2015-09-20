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
#include "page_replacement.h"
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

int pkg_size, pkg_num;
bool nfs_pkg = false; 

static inline unsigned CONST umin(unsigned a, unsigned b) {
    return (a < b) ? a : b;
}

static inline unsigned CONST umax(unsigned a, unsigned b) {
    return (a > b) ? a : b;
}

timestamp_t start_time, end_time;

static void iov_free(iovec_t *iov) {
    iovec_t *cur;
    while(iov) {
        cur = iov;
        iov = iov->next;
        free(cur);
    }
}

void syscall_end_continuation(sos_proc_t *proc, int retval, bool success) {
    if (nfs_pkg) {
        dprintf(4, "pkg_size = %d\n", pkg_size);
        nfs_pkg = false;
    }
    seL4_MessageInfo_t reply;
    dprintf(4, "ENDING SYSCALL\n", retval);
    dprintf(4, "[SYSEND] Returning %d\n", retval);
    size_t message_length = umax(proc->cont.reply_length, 1);
    if (success) {
        reply = seL4_MessageInfo_new(seL4_NoFault, 0, 0, message_length);
    } else {
        reply = seL4_MessageInfo_new(seL4_UserException, 0, 0, message_length);
    }
    seL4_SetMR(0, retval);
    seL4_SetTag(reply);
    assert(proc->cont.reply_cap != seL4_CapNull);
    seL4_Send(proc->cont.reply_cap, reply);
    iov_free(proc->cont.iov);
    memset(&proc->cont, 0, sizeof(cont_t));
    dprintf(4, "SYSCALL ENDED\n", retval);
}

static bool check_region(sos_addrspace_t *as, client_vaddr page, iop_direction_t dir) {
    sos_region_t* reg = as_vaddr_region(as, page);
    if (!reg) {
        return false;
    }
    // Ensure client process has correct permissions to the page
    if (!(reg->rights & seL4_CanWrite) && dir == WRITE) {
        return false;
    } else if (!(reg->rights & seL4_CanRead) && dir == READ) {
        return false;
    }
    return true;
}

/**
 * Pack characters of the message into seL4_words
 * @param msgdata message to be printed
 * @param count length of the message
 * @returns the number of characters encoded
 */
void ipc_write(int start, char* msgdata, size_t length) {
    size_t mr_idx,i,j;
    seL4_Word pack;
    i = 0;
    seL4_SetMR(start, length);
    mr_idx = start + 1;
    while(i < length) {
        pack = 0;
        j = sizeof(seL4_Word);
        while (j > 0 && i < length) {
            pack = pack | ((seL4_Word)msgdata[i] << ((--j)*8));
            i++;
        }
        seL4_SetMR(mr_idx, pack);
        mr_idx++;
    }
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
    if (k < MAX_FILE_PATH_LENGTH) {
        buf[k] = 0;
    }
}



void iov_ensure_loaded(iovec_t* iov) {
    sos_addrspace_t *as = current_process()->vspace;
    sos_region_t *reg = as_vaddr_region(as, iov->vstart);
    if (as_page_exists(as, iov->vstart)) {
        if (swap_is_page_swapped(as, iov->vstart)) { // page is in disk
            swap_replace_page(as, iov->vstart);
        } else if (!is_referenced(as, iov->vstart)) {
            as_reference_page(as, iov->vstart, reg->rights);
        }
    } else {
        assert(reg);
        process_create_page(iov->vstart, reg->rights);
    }
}

static iovec_t* iov_create(client_vaddr vstart, size_t sz, iovec_t *iohead, iovec_t *iotail) {
    iovec_t *ionew = malloc(sizeof(iovec_t));
    if (ionew == NULL) {
        iov_free(iohead);
        return NULL;
    }
    ionew->vstart = vstart;
    ionew->sz = sz;
    ionew->next = NULL;

    if (iohead == NULL) {
        return ionew;
    } else {
        iotail->next = ionew;
        return iohead;
    }
}

iovec_t *cbuf_to_iov(client_vaddr buf, size_t nbyte, iop_direction_t dir) {
    bool vstart_okay;
    size_t remaining = nbyte;
    iovec_t *iohead = NULL;
    iovec_t *iotail = NULL;
    sos_addrspace_t* as = current_as();
    if (remaining == 0) {
        vstart_okay = check_region(as, buf, dir);
        if (vstart_okay == false) {
            ERR("Client page lookup %x failed\n", buf);
            return NULL;
        }
        iohead = iov_create(buf, 0, NULL, NULL);
        return iohead;
    }
    dprintf(1, "cbuf_to_iov: %d bytes\n", nbyte);
    while(remaining) {
        size_t offset = ((unsigned)buf % PAGE_SIZE);
        size_t buf_delta = umin((PAGE_SIZE - offset), remaining);
        vstart_okay = check_region(as, buf, dir);
        if (vstart_okay == false) {
            ERR("Client page lookup %x failed\n", buf);
            return NULL;
        }
        dprintf(1, "cbuf_to_iov: delta=%d\n", buf_delta);
        iohead = iov_create(buf, buf_delta, iohead, iotail);
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

io_device_t* device_handler_str(const char* filename) {
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

int sos__sys_open(void) {
    const char *path = current_process()->cont.path;
    fmode_t mode = current_process()->cont.file_mode;
    io_device_t *dev = device_handler_str(path);
    // TODO: Ensure all arguments are configured (for this and all other
    // syscalls)
    assert(dev);
    return dev->open(path, mode);
}

int sos__sys_read(void){
    int file = current_process()->cont.fd;
    size_t nbyte = current_process()->cont.length_arg;
    if (nbyte == 0) {
        syscall_end_continuation(current_process(), 0, true);
        return 0;
    }
    of_entry_t *of = fd_lookup(current_process(), file);
    if (of == NULL) {
        return 1;
    }
    if (!(of->mode & FM_READ)) {
        return EPERM;
    }
    io_device_t *dev = device_handler_fd(file);
    assert(dev);
    return dev->read(current_process()->cont.iov, file, nbyte);
}

int sos__sys_write(void) {
    int file = current_process()->cont.fd;
    size_t nbyte = current_process()->cont.length_arg;
    of_entry_t *of = fd_lookup(current_process(), file);
    if (of == NULL) {
        return 1;
    }
    if (!(of->mode & FM_WRITE)) {
        return EPERM;
    }
    if (nbyte == 0) {
        syscall_end_continuation(current_process(), 0, true);
        return 0;
    }
    io_device_t *dev = device_handler_fd(file);
    assert(dev);
    start_time = time_stamp();
    return dev->write(current_process()->cont.iov, file,nbyte);
}

int sos__sys_stat(void) {
    return nfs_io.stat();
}

int sos__sys_getdirent(void) {
    return nfs_io.getdirent();
}

/**
 * Close an open file.
 * Should not block the caller.
 */
int sos__sys_close(void) {
    int file = current_process()->cont.fd;
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
