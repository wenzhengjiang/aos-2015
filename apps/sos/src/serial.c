#include "serial.h"
#include <string.h>
#include <stdlib.h>

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

void sys_serial_open(void) {
    serial = serial_init();
    serial_register_handler(serial, serial_handler);
}

int serial_read(iovec_t* vec, size_t nbyte) {
    int pos = 0;
    for (iovec_t *v = vec; v && pos < buflen; v = v->next) {
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

int serial_write(iovec_t* vec, size_t nbyte) {
    int sent = 0;
   for (iovec_t *v = vec; v ; v = v->next) {
       sent += serial_send(serial, (char*)v->start, v->sz);
   }
   return sent;
}
