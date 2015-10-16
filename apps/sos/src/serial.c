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
static char line_buf[SERIAL_BUF_SIZE];
static size_t line_buflen;
static int bid = 0; // current line buffer

static int reader_pid;
extern bool callback_done;
static inline unsigned CONST min(unsigned a, unsigned b)
{
    return (a < b) ? a : b;
}

static inline void try_send_buffer() {
    dprintf(3, "[SERIAL] Attempting 'try_send_buffer'\n");
    if (reader_pid == 0) {
        return;
    }
    sos_proc_t *proc = process_lookup(reader_pid);
    assert(proc);
    if (line_buflen == 0 || proc->cont.reply_cap == seL4_CapNull)
        return ;
    if (proc->cont.syscall_number != SOS_SYSCALL_READ )
        return;
    if (!proc->cont.iov) {
        printf("process %d was broken\b", reader_pid);
        assert(proc->cont.iov);
    }
    char *buf = line_buf;
    int buflen = line_buflen;
    int pos = 0;
    dprintf(4, "[SERIAL] Checking iovs\n");
    for (iovec_t *v = proc->cont.iov; v && pos < buflen; v = v->next) {
        assert(v->sz);
        pte_t* pt = as_lookup_pte(proc->vspace, v->vstart);
        if (pt == NULL) {
            // The process has been killed.  We no longer care.
            ERR("Read failure.  Reader has been killed(?)\n");
            return;
        }
        int n = min(buflen - pos, v->sz);
        sos_vaddr dst = as_lookup_sos_vaddr(proc->vspace, v->vstart);
        assert(dst);
        memcpy((char*)dst, buf+pos, n);
        pos += n;
        // unpin the page
        pt->pinned = false;
    }
    // reply to client reader
    syscall_end_continuation(proc, pos, true);

    if (pos < buflen) {
        memmove(buf, buf + pos, buflen - pos);
    }
    line_buflen -= pos;
}

static void serial_handler(struct serial *serial, char c) {
    assert(serial);
    line_buf[line_buflen++] = c;

    if (c == '\n' || line_buflen == SERIAL_BUF_SIZE) {
        try_send_buffer();
    }
}


int sos_serial_close(int fd) {
    (void)fd;
    // TODO: Commented things should probably be in a destroy()-like function
    //serial_register_handler(serial, NULL);
    //serial = NULL;
    if (effective_process()->fd_table[fd] == NULL) { return ENOENT; }
    if (effective_process()->fd_table[fd]->mode & FM_READ) {
        reader_pid = 0;
    }
    //line_buflen[0] = line_buflen[1] = 0;
    fd_free(effective_process()->fd_table, fd);
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
    assert(vec);
    sos_proc_t* proc = current_process();
    assert(proc != NULL);
    cont_t *cont = &(proc->cont);
    cont->iov = vec;
    for (; vec != NULL; vec = vec->next) {
        sos_region_t* reg = as_vaddr_region(current_process()->vspace, vec->vstart);
        if (!reg) {
            process_delete(current_process());
        }
        // TODO: Needs refactor as codeblock appears a few times thruout SOS
        if (as_page_exists(current_process()->vspace, vec->vstart)) {
            dprintf(4, "page exists\n");
            if (swap_is_page_swapped(current_process()->vspace, vec->vstart)) { // page is in disk
                swap_replace_page(vec->vstart);
                as_reference_page(current_process()->vspace, vec->vstart, reg->rights);
                current_process()->cont.page_eviction_process = NULL;
            } else if (!is_referenced(current_process()->vspace, vec->vstart)) {
                as_reference_page(current_process()->vspace, vec->vstart, reg->rights);
            }
        } else {
            dprintf(4, "create new page\n");
            process_create_page(vec->vstart, reg->rights);
        }
        pte_t* pt = as_lookup_pte(current_process()->vspace, vec->vstart);
        pt->pinned = true;
    }

    // try to send latest line buffer which is ready to be sent
    if (line_buflen > 0) {
        try_send_buffer();
    }
    return 0;
}

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
    printf("ending continuation\n");
    syscall_end_continuation(current_process(), sent, true);
    return 0;
}

void sos_serial_init() {
    serial = serial_init();
    serial_register_handler(serial, serial_handler);
}
