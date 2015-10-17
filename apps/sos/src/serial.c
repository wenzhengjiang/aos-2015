/**
 * @file serial.c
 * @brief Implementation of serial IO interface
 */

#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>
#include <errno.h>
#include <syscall.h>
#include <sel4/sel4.h>
#include "serial.h"
#include "file.h"
#include "process.h"
#include "page_replacement.h"

#define SERIAL_BUF_SIZE  1024

#define verbose 5
#include <log/debug.h>
#include <log/panic.h>

// Note: even though we DO have a close function, this should NOT be exposed
// to the client
io_device_t serial_io = {
    .open = sos_serial_open,
    .close = sos_serial_close,
    .read = sos_serial_read,
    .write = sos_serial_write,
    .getdirent = NULL,
    .stat = NULL
};

static struct serial* serial;
static char line_buf[SERIAL_BUF_SIZE];
static size_t line_buflen;

static int reader_pid;
static inline unsigned CONST min(unsigned a, unsigned b)
{
    return (a < b) ? a : b;
}

/**
 * @brief   Send line buffer to console reader
 *          1. No console reader - do nothing
 *          2. Reader is already dead - do nothing
 *          3. Reader is not reading  - do nothing
 */
// TODO serial read interrupt is same as nfs read callback except read all data once(?)
static inline void try_send_buffer() {
    dprintf(3, "[SERIAL] Attempting 'try_send_buffer'\n");
    if (reader_pid == 0) {
        return;
    }
    sos_proc_t *proc = process_lookup(reader_pid);
    if (proc == NULL) {
        return ;
    }
    if (proc->cont.syscall_number != SOS_SYSCALL_READ )
        return;
    assert(line_buflen > 0 );
    assert(proc->cont.iov); 

    char *buf = line_buf;
    int buflen = line_buflen;
    int pos = 0;
    dprintf(4, "[SERIAL] Checking iov\n");
    for (iovec_t *v = proc->cont.iov; v && pos < buflen; v = v->next) {
        assert(v->sz);
        
        int n = min(buflen - pos, v->sz);
        sos_vaddr dst = as_lookup_sos_vaddr(proc->vspace, v->vstart);
        assert(dst);
        memcpy((char*)dst, buf+pos, n);
        pos += n;
        // unpin the page
        pte_t* pt = as_lookup_pte(proc->vspace, v->vstart);
        assert(pt);
        pt->pinned = false;
    }
    // reply to client reader
    syscall_end_continuation(proc, pos, true);

    if (pos < buflen) {
        memmove(buf, buf + pos, buflen - pos);
    }
    line_buflen -= pos;
}

/**
 * @brief serial interrupt handler
 *
 */
static void serial_handler(struct serial *serial, char c) {
    assert(serial);
    line_buf[line_buflen++] = c;

    if (c == '\n' || line_buflen == SERIAL_BUF_SIZE) {
        try_send_buffer();
    }
}

/**
 * @brief Clear consoler reader state and free the fd item
 *
 * @param fd
 *
 * @return 
 */
int sos_serial_close(int fd) {
    if (effective_process()->fd_table[fd] == NULL) { return ENOENT; }
    if (effective_process()->fd_table[fd]->mode & FM_READ) {
        reader_pid = 0;
    }
    fd_free(effective_process()->fd_table, fd);
    return 0;
}

/**
 * @brief set current reader of console to current process
 *
 * @param filename can only be "console"
 * @param mode
 *
 * @return  error code
 */
int sos_serial_open(const char* filename, fmode_t mode) {
    dprintf(3, "[SERIAL] sos_serial_open()\n");
    assert(!strcmp(filename, "console"));
    sos_proc_t* proc = current_process();
    int err = 0;
    if (mode & FM_READ) {
        if (reader_pid == 0) { // No process is reading console
            reader_pid = proc->pid;
        } else { // Another process is reading console
            dprintf(4, "[SERIAL] Busy reader: owned by %d\n", reader_pid);
            err = EBUSY;
            fd_free(proc->fd_table, proc->cont.fd);
        }
    }
    dprintf(4, "[SERIAL] sos_serial_open() complete\n");
    if (err) return err;
    else {
        syscall_end_continuation(current_process(), proc->cont.fd, true);
        return 0;
    }
}

/**
 * @brief   Ensure pages in iov is in memory and pin them, so when interrupt triggered, they are still there
 *          If line buffer is not empty, return client with data in line buffer 
 *
 * @return error code
 */
int sos_serial_read(iovec_t* vec, int fd, int count) {
    (void)count;
    assert(vec);
    sos_proc_t* proc = current_process();
    assert(proc != NULL);
    cont_t *cont = &(proc->cont);
    cont->iov = vec;
    for (; vec != NULL; vec = vec->next) {
        iov_ensure_loaded(*vec); 
        pte_t* pt = as_lookup_pte(current_process()->vspace, vec->vstart);
        pt->pinned = true;
    }

    // try to send latest line buffer which is ready to be sent
    if (line_buflen > 0) {
        try_send_buffer();
    }
    return 0;
}

/**
 * @brief Send page content in iov pages to serial console
 *
 * @return error code
 */
int sos_serial_write(iovec_t* vec, int fd, int count) {
    dprintf(3, "[SERIAL] Starting serial_write %d, %d, %d\n", current_process()->pid, fd, count);
    (void)fd;
    (void)count;
    int sent = 0;
    vec = current_process()->cont.iov;
    assert(vec);

    for (iovec_t *v = vec; v ; v = v->next) {
        iov_ensure_loaded(*v);
        if (v->sz == 0) return 0;
        sos_vaddr src = as_lookup_sos_vaddr(current_process()->vspace, v->vstart);
        assert(src);
        sent += serial_send(serial, (char*)src, v->sz);
    }
    syscall_end_continuation(current_process(), sent, true);
    return 0;
}

void sos_serial_init() {
    serial = serial_init();
    serial_register_handler(serial, serial_handler);
}
