#include "serial.h"
#include <string.h>
#include <stdlib.h>
#include <assert.h>

#define SERIAL_BUF_SIZE  1024

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

void sos_serial_open(void) {
    serial = serial_init();
    serial_register_handler(serial, serial_handler);
}

int sos_serial_read(iovec_t* vec, size_t nbyte) {
    assert(vec && nbyte);
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

int sos_serial_write(iovec_t* vec, size_t nbyte) {
    assert(vec && nbyte);
    assert(serial);
    int sent = 0;
    for (iovec_t *v = vec; v ; v = v->next) {
        assert(vec->sz);
        sent += serial_send(serial, (char*)v->start, v->sz);
    }
    return sent;
}
