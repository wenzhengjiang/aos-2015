#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>
#include <sel4/sel4.h>
#include "serial.h"
#include "io_device.h"
#include "syscall.h"
#define SERIAL_BUF_SIZE  1024

#define verbose 5
#include <log/debug.h>
#include <log/panic.h>


io_device_t serial_io = {
    .open = sos_serial_open,
    .close = sos_serial_close,
    .read = sos_serial_read,
    .write = sos_serial_write
};

#define NEXT_BID(i) ((i+1)%2)
#define PREV_BID(i) ((i+1)%2)

static struct serial* serial;
static char line_buf[2][SERIAL_BUF_SIZE];
static size_t line_buflen[2];
static int bid = 0; // current line buffer

extern seL4_CPtr reader_cap;
iovec_t *reader_iov;

static inline unsigned CONST min(unsigned a, unsigned b)
{
    return (a < b) ? a : b;
}

static inline void try_send_buffer(int i) {
    if (line_buflen[i] == 0 || reader_cap == 0)
        return ;
    assert(reader_iov);
    char *buf = line_buf[i];
    int buflen = line_buflen[i];
    int pos = 0;
    for (iovec_t *v = reader_iov; v && pos < buflen; v = v->next) {
        assert(v->sz);
        int n = min(buflen-pos, v->sz);
        memcpy((char*)v->start, buf+pos, n); 
        pos += n;
    }
    // reply to client reader
    seL4_MessageInfo_t reply = seL4_MessageInfo_new(seL4_NoFault,0,0,1);
    seL4_SetMR(0, pos);
    seL4_Send(reader_cap, reply);
    cspace_free_slot(cur_cspace, reader_cap);
    reader_cap = 0;
    iov_free(reader_iov);

    if (pos < buflen) {
        memmove(buf, buf+pos, buflen-pos);
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


int sos_serial_close(void) {
    serial_register_handler(serial, NULL);
    serial = NULL;
    line_buflen[0] = line_buflen[1] = 0;
    return 0;
}

int sos_serial_open(void) {
    serial = serial_init();
    serial_register_handler(serial, serial_handler);
    return 5;
}

int sos_serial_read(iovec_t* vec) {
    assert(vec);
    reader_iov = vec;
    return 0;
}

int sos_serial_write(iovec_t* vec) {
    assert(vec);
    if (!serial) {
        sos_serial_open();
    }
    int sent = 0;
    for (iovec_t *v = vec; v ; v = v->next) {
        assert(vec->sz);
        sent += serial_send(serial, (char*)v->start, v->sz);
    }
    return sent;
}

