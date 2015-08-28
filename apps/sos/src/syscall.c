#include <sel4/sel4.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>
#include <string.h>
#include <serial/serial.h>
#include <cspace/cspace.h>
#include <limits.h>
#include "frametable.h"
#include "process.h"
#include "addrspace.h"
#include <assert.h>

#define verbose 5
#include <log/debug.h>
#include <log/panic.h>

#define PRINT_MESSAGE_START 2
#define SERIAL_BUF_SIZE  1024

typedef enum iop_direction {READ, WRITE} iop_direction_t;

static struct serial* serial;
static char ser_buf[SERIAL_BUF_SIZE];
static size_t ser_buflen = 0;

static inline unsigned CONST umin(unsigned a, unsigned b)
{
    return (a < b) ? a : b;
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

static void serial_handler(struct serial *serial, char c) {
    if (ser_buflen == SERIAL_BUF_SIZE) return; // TODO better solution not losing data ?
    ser_buf[ser_buflen++] = c;
}



void sys_serial_open(void) {
    serial = serial_init();
    serial_register_handler(serial, serial_handler);
}

static sos_vaddr
check_page(sos_addrspace_t *as, char* buf, iop_direction_t dir) {
    // Ensure client process has the page mapped
    sos_vaddr saddr = as_lookup_sos_vaddr(as, (client_vaddr)buf);
    if (saddr == 0) {
        return 0;
    }
    // Ensure client process has correct permissions to the page
    sos_region_t* reg = as_vaddr_region(as, (client_vaddr)buf);
    if (!(reg->rights & seL4_CanWrite) && dir == WRITE) {
        return 0;
    } else if (!(reg->rights & seL4_CanRead)) {
        return 0;
    }
    return saddr;
}

static int check_iop(char* buf, size_t nbyte, iop_direction_t dir) {
    size_t remaining = nbyte;
    sos_addrspace_t* as = current_as();
    while(remaining) {
        size_t buf_delta = (PAGE_SIZE - ((unsigned)buf % PAGE_SIZE));
        sos_vaddr saddr = check_page(as, buf, dir);
        if (saddr == 0) {
            return 1;
        }
        remaining -= buf_delta;
        buf += buf_delta;
    }
    return 0;
}

/**
 * @param num_args number of IPC args supplied
 * @returns length of the message printed
 */
size_t sys_print(size_t num_args) {
    size_t i,unpack_len,send_len;
    size_t total_unpack = 0;
    seL4_Word packed_data;
    char *msgBuf = malloc(seL4_MsgMaxLength * sizeof(seL4_Word));
    char *bufPtr = msgBuf;
    char req_count = seL4_GetMR(1);
    memset(msgBuf, 0, seL4_MsgMaxLength * sizeof(seL4_Word));
    for (i = 0; i < num_args - PRINT_MESSAGE_START + 1; i++) {
        packed_data = seL4_GetMR(i + PRINT_MESSAGE_START);
        unpack_len = unpack_word(bufPtr, packed_data);
        total_unpack += unpack_len;
        bufPtr += unpack_len;
        /* Unpack was short the expected amount, so we signal the end. */
        if (unpack_len < sizeof(seL4_Word)) {
            break;
        }
    }
    send_len = serial_send(serial, msgBuf, umin(req_count, total_unpack));
    free(msgBuf);
    return send_len;
}

// copy content in serial buffer to buf
// buf is a sosptr
int sys_serial_read(client_vaddr cbuf, size_t nbyte) {
    assert(buf);
    char *buf = (char*) cbuf;
    size_t remaining = nbyte;
    sos_addrspace_t* as = current_as();
    if (check_iop(buf, nbyte, READ) != 0) {
        // TODO: Use real error code / kill process
        return -1;
    }
    while(remaining > 0) {
        sos_vaddr saddr = as_lookup_sos_vaddr(as, (client_vaddr)buf);
        size_t offset = ((unsigned)buf % PAGE_SIZE);
        size_t buf_delta = (PAGE_SIZE - offset);
        size_t buf_actual = umin(buf_delta, ser_buflen);
        sos_map_frame(saddr);
        memcpy(buf + offset, ser_buf, buf_actual);
        sos_unmap_frame(saddr);
        ser_buflen -= buf_actual;
        if (ser_buflen > 0) {
            memmove(ser_buf, ser_buf + buf_actual, ser_buflen - buf_actual);
        }
        buf += buf_actual;
    }
    assert(remaining == 0);
    return (int)(nbyte - remaining);
}

int sys_serial_write(client_vaddr buf, size_t nbyte) {
    assert(buf && nbyte);
    return serial_send(serial, (char*)buf, nbyte);
}
