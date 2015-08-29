#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include "serial.h"
#include "io_device.h"
#define SERIAL_BUF_SIZE  1024

io_device_t serial_io = {
    &sos_serial_open,
    &sos_serial_close,
    &sos_serial_read,
    &sos_serial_write
};

static struct serial* serial;
static char buf[SERIAL_BUF_SIZE];
static size_t buflen = 0;

static inline unsigned CONST min(unsigned a, unsigned b)
{
    return (a < b) ? a : b;
}

static void serial_handler(struct serial *serial, char c) {
    if (buflen == SERIAL_BUF_SIZE) return; // TODO better solution not losing data ?
    buf[buflen++] = c;
}

int sos_serial_close(void) {
    serial_register_handler(serial, NULL);
    serial = NULL;
    return 0;
}

int sos_serial_open(char* pathname, fmode_t mode) {
    (void)pathname;
    (void)mode;
    serial = serial_init();
    serial_register_handler(serial, serial_handler);
    return 0;
}

int sos_serial_read(iovec_t* vec) {
    assert(vec);
    int pos = 0;
    for (iovec_t *v = vec; v && pos < buflen; v = v->next) {
        assert(vec->sz);
        int n = min(buflen-pos, vec->sz);
        memcpy((char*)v->start, buf+pos, n); 
        pos += n;
    }
    if (pos < buflen) {
        memmove(buf, buf+pos, buflen-pos);
    }
    buflen -= pos;
    return pos;
}

int sos_serial_write(iovec_t* vec) {
    assert(vec);
    assert(serial);
    int sent = 0;
    for (iovec_t *v = vec; v ; v = v->next) {
        assert(vec->sz);
        sent += serial_send(serial, (char*)v->start, v->sz);
    }
    return sent;
}
