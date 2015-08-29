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

#define MAX_FILE_PATH_LENGTH (2048)

typedef enum iop_direction {READ, WRITE} iop_direction_t;

typedef enum device_type {SERIAL, UNKNOWN} device_type_t;

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
    } else if (!(reg->rights & seL4_CanRead)) {
        return 0;
    }
    return saddr;
}

static void iov_free(iovec_t *iov) {
    iovec_t *cur;
    while(iov) {
        cur = iov;
        iov = iov->next;
        free(cur);
    }
}

static iovec_t* iov_create(size_t offset, size_t buf_delta, iovec_t *iohead, iovec_t **iotail) {
    iovec_t *ionew = malloc(sizeof(iovec_t));
    if (ionew == NULL) {
        iov_free(iohead);
        return NULL;
    }
    ionew->start = offset;
    ionew->sz = buf_delta;
    ionew->next = NULL;
    (*iotail)->next = ionew;
    *iotail = ionew;
    if (iohead == NULL) {
        iohead = ionew;
    }
    return iohead;
}

static iovec_t *buf_to_iov(client_vaddr buf, size_t nbyte, iop_direction_t dir) {
    size_t remaining = nbyte;
    iovec_t *iohead, *iotail;
    sos_addrspace_t* as = current_as();
    while(remaining) {
        size_t offset = ((unsigned)buf % PAGE_SIZE);
        size_t buf_delta = (PAGE_SIZE - offset);
        sos_vaddr saddr = check_page(as, buf, dir);
        if (saddr == 0) {
            ERR("Client page lookup %x failed\n", buf);
            return NULL;
        }
        iohead = iov_create(offset, buf_delta, iohead, &iotail);
        if (iohead == NULL) {
            ERR("Insufficient memory to create new iovec\n");
            return NULL;
        }
        remaining -= buf_delta;
        buf += buf_delta;
    }
    return iohead;
}

static device_type_t get_device_type(char* filename) {
    if (strcmp(filename, "console") == 0) {
        return SERIAL;
    }
    return UNKNOWN;
}

static io_device_t* device_handler(char* filename) {
    device_type_t dev = get_device_type(filename);
    switch (dev) {
    case SERIAL:
        return &serial_io;
    case UNKNOWN:
        ERR("UNKNOWN DEVICE TYPE");
        return NULL;
    }
    ERR("Unhandled device");
    assert(false);
    // Will never happen
    return NULL;
}

int sos__sys_open(client_vaddr path, fmode_t mode, int *ret) {
    (void)path;
    (void)mode;
    (void)ret;
    return 0;
}

int sos__sys_read(int file, client_vaddr buf, size_t nbyte, int *ret){
    (void)buf;
    (void)nbyte;
    (void)ret;
    return 0;
}

int sos__sys_write(int file, client_vaddr buf, size_t nbyte, int *ret) {
    (void)buf;
    (void)nbyte;
    (void)ret;
    return 0;
}
