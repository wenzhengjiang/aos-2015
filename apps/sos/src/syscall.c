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
#include <sos.h>

#define verbose 5
#include <log/debug.h>
#include <log/panic.h>

#define PRINT_MESSAGE_START 2

typedef enum iop_direction {READ, WRITE} iop_direction_t;

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

int sos__sys_open(client_vaddr path, fmode_t mode, int *ret) {
    (void)path;
    (void)mode;
    (void)ret;
    return 0;
}

int sos__sys_read(int file, char *buf, size_t nbyte, int *ret){
    (void)buf;
    (void)nbyte;
    (void)ret;
    return 0;
}

int sos__sys_write(int file, char *buf, size_t nbyte, int *ret) {
    (void)buf;
    (void)nbyte;
    (void)ret;
    return 0;
}
