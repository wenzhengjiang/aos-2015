#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>
#include <errno.h>
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

#define NEXT_BID(i) ((i+1)%2)
#define PREV_BID(i) ((i+1)%2)

static struct serial* serial;
static char line_buf[2][SERIAL_BUF_SIZE];
static size_t line_buflen[2];
static int bid = 0; // current line buffer

static int reader_pid;
extern bool callback_done;
static inline unsigned CONST min(unsigned a, unsigned b)
{
    return (a < b) ? a : b;
}

static inline void try_send_buffer(int i) {
    dprintf(3, "[SERIAL] Attempting 'try_send_buffer'\n");
    if (reader_pid == 0) {
        return;
    }
    sos_proc_t *proc = process_lookup(reader_pid);
    assert(proc);
    if (line_buflen[i] == 0 || proc->cont.reply_cap == seL4_CapNull)
        return ;
    assert(proc->cont.iov);
    char *buf = line_buf[i];
    int buflen = line_buflen[i];
    int pos = 0;
    dprintf(4, "[SERIAL] Checking iovs\n");
    for (iovec_t *v = proc->cont.iov; v && pos < buflen; v = v->next) {
        assert(v->sz);
        int n = min(buflen - pos, v->sz);
        sos_vaddr dst = as_lookup_sos_vaddr(proc->vspace, v->vstart);
        assert(dst);
        memcpy((char*)dst, buf+pos, n);
        pos += n;
        pte_t* pt = as_lookup_pte(current_process()->vspace, v->vstart);
        // Pin the page
        pt->pinned = true;
    }
    // reply to client reader
    syscall_end_continuation(current_process(), pos, true);

    if (pos < buflen) {
        memmove(buf, buf + pos, buflen - pos);
    }
    line_buflen[i] -= pos;
}

static void serial_handler(struct serial *serial, char c) {
    assert(serial);
    line_buf[bid][line_buflen[bid]++] = c;

    if (c == '\n' || line_buflen[bid] == SERIAL_BUF_SIZE) {  // switch to next line buffer
        bid = NEXT_BID(bid);
        line_buflen[bid] = 0;
    }
    // try to send latest line buffer which is ready to be sent
    try_send_buffer(PREV_BID(bid));
}


int sos_serial_close(int fd) {
    (void)fd;
    // TODO: Commented things should probably be in a destroy()-like function
    //serial_register_handler(serial, NULL);
    //serial = NULL;
    if (current_process()->fd_table[fd] == NULL) { return ENOENT; }
    if (current_process()->fd_table[fd]->mode & FM_READ) {
        reader_pid = 0;
    }
    //line_buflen[0] = line_buflen[1] = 0;
    fd_free(current_process()->fd_table, fd);
    return 0;
}

int sos_serial_open(const char* filename, fmode_t mode) {
    dprintf(3, "[SERIAL] sos_serial_open()\n");
    assert(!strcmp(filename, "console"));
    //    line_buflen[0] = line_buflen[1] = 0; // clear buffer
    sos_proc_t* proc = current_process();
    int retval;
    if (mode & FM_READ) {
        if (reader_pid == 0) {
            reader_pid = proc->pid;
            retval = proc->cont.fd;
        } else {
            dprintf(4, "[SERIAL] Busy reader: owned by %d\n", reader_pid);
            retval = -EBUSY;
            fd_free(proc->fd_table, proc->cont.fd);
        }
    } else {
        retval = proc->cont.fd;
    }

    if (retval >= 0) {
        syscall_end_continuation(current_process(), retval, true);
    } else {
        syscall_end_continuation(current_process(), retval, false);
    }
    dprintf(4, "[SERIAL] sos_serial_open() complete\n");
    return 0;
}

int sos_serial_read(iovec_t* vec, int fd, int count) {
    (void)count;
    sos_proc_t* proc = current_process();
    assert(proc != NULL);
    cont_t *cont = &(proc->cont);
    cont->iov = vec;
    for (; vec != NULL; vec = vec->next) {
        sos_region_t* reg = as_vaddr_region(current_process()->vspace, vec->vstart);
        // TODO: kill the client
        assert(reg);
        pte_t* pt = as_lookup_pte(current_process()->vspace, vec->vstart);
        // Pin the page
        pt->pinned = false;
        // TODO: Needs refactor as codeblock appears a few times thruout SOS
        if (as_page_exists(current_process()->vspace, vec->vstart)) {
            dprintf(4, "page exists\n");
            if (swap_is_page_swapped(current_process()->vspace, vec->vstart)) { // page is in disk
                swap_replace_page(current_process()->vspace, vec->vstart);
            } else if (!is_referenced(current_process()->vspace, vec->vstart)) {
                as_reference_page(current_process()->vspace, vec->vstart, reg->rights);
            }
        } else {
            dprintf(4, "create new page\n");
            process_create_page(vec->vstart, reg->rights);
        }
    }
    return 0;
}

int sos_serial_write(iovec_t* vec, int fd, int count) {
    dprintf(3, "[SERIAL] Starting serial_write\n");
    (void)fd;
    (void)count;
    assert(vec);
    int sent = 0;
    for (iovec_t *v = vec; v ; v = v->next) {
        assert(vec->sz);
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
