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
#include "process.h"
#include "addrspace.h"
#include "syscall.h"
#include "io_device.h"
#include <assert.h>
#include <sos.h>

#define verbose 5
#include <log/debug.h>
#include <log/panic.h>

#define PRINT_MESSAGE_START (2)

extern io_device_t serial_io;
device_map_t dev_map[DEVICE_NUM] = {{&serial_io, "console"}};

typedef enum iop_direction {READ, WRITE} iop_direction_t;

static inline unsigned CONST umin(unsigned a, unsigned b)
{
    return (a < b) ? a : b;
}

static sos_vaddr
check_page(sos_addrspace_t *as, client_vaddr buf, iop_direction_t dir) {
    // Ensure client process has the page mapped
    sos_vaddr saddr = as_lookup_sos_vaddr(as, buf);
    if (saddr == 0) {
        return 0;
    }
    // Ensure client process has correct permissions to the page
    sos_region_t* reg = as_vaddr_region(as, buf);
    if (!(reg->rights & seL4_CanWrite) && dir == WRITE) {
        return 0;
    } else if (!(reg->rights & seL4_CanRead) && dir == READ) {
        return 0;
    }
    return saddr;
}

void iov_free(iovec_t *iov) {
    iovec_t *cur;
    while(iov) {
        cur = iov;
        iov = iov->next;
        free(cur);
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
    int k = 0;
    for (int i = start; i < seL4_MsgMaxLength; i++) {
       int len = unpack_word(buf+k, seL4_GetMR(i)); 
       k += len;
       if (k < sizeof(seL4_Word)) break;
    }
    if (k < MAX_FILE_PATH_LENGTH)
        buf[k] = 0;
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
    iovec_t iov = { .start = (sos_vaddr)msgBuf,
                    .sz = umin(req_count, total_unpack), 
                    .next = NULL};
    send_len = sos_serial_write(&iov);
    free(msgBuf);
    return send_len;
}

static iovec_t* iov_create(size_t start, size_t sz, iovec_t *iohead, iovec_t **iotail) {
    printf("iov_create: %u, %u\n", start ,sz);
    iovec_t *ionew = malloc(sizeof(iovec_t));
    if (ionew == NULL) {
        iov_free(iohead);
        return NULL;
    }
    ionew->start = start;
    ionew->sz = sz;
    ionew->next = NULL;
    if (iohead == NULL) {
        return ionew;
    } else {
        assert(*iotail);
        (*iotail)->next = ionew;
        *iotail = ionew;
        return iohead;
    }
}

static iovec_t *cbuf_to_iov(client_vaddr buf, size_t nbyte, iop_direction_t dir) {
    size_t remaining = nbyte;
    iovec_t *iohead = NULL;
    iovec_t **iotail = &iohead;
    sos_addrspace_t* as = current_as();
    while(remaining) {
        size_t offset = ((unsigned)buf % PAGE_SIZE);
        size_t buf_delta = umin((PAGE_SIZE - offset), remaining);
        sos_vaddr saddr = check_page(as, buf, dir);
        if (saddr == 0) {
            ERR("Client page lookup %x failed\n", buf);
            return NULL;
        }
        iohead = iov_create(saddr+offset, buf_delta, iohead, iotail);
        if (iohead == NULL) {
            ERR("Insufficient memory to create new iovec\n");
            return NULL;
        }
        remaining -= buf_delta;
        buf += buf_delta;
    }
    return iohead;
}

static io_device_t* device_handler(const char* filename) {
    for (int i = 0; i < DEVICE_NUM; i++) {
        if (strcmp(filename, dev_map[i].name) == 0)
            return dev_map[i].handler;
    }
    return NULL;
}

int sos__sys_open(const char *path, fmode_t mode, int *ret) {
    printf("Open %s\n", path);
    io_device_t *dev = device_handler(path); //TODO removd hardcode
    if (dev) {
        *ret = dev->open();
    } else 
        assert(!"only support console");

    return 0;
}

int sos__sys_read(int file, client_vaddr buf, size_t nbyte, int *ret){
    printf("sos__sys_read: enter %08x\n", buf);
    io_device_t *dev = device_handler("console"); //TODO removd hardcode
    iovec_t *iov = cbuf_to_iov(buf, nbyte, READ);
    if (iov == NULL) {
        assert(!"illegal buf addr");
        return EINVAL;
    }
    if (dev) {
        *ret = dev->read(iov);
    } else 
        assert(!"only support console");
    printf("sos__sys_read: leave\n");
    return 0;
}

int sos__sys_write(int file, client_vaddr buf, size_t nbyte, int *ret) {
    io_device_t *dev = device_handler("console"); //TODO removd hardcode
    iovec_t *iov = cbuf_to_iov(buf, nbyte, WRITE);
    if (dev) {
        *ret = dev->write(iov);
    } else 
        assert(!"only support console");
    iov_free(iov);
    return 0;
}
