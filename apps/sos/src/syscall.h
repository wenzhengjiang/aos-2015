#ifndef _SOS_SYSCALL_H_
#define _SOS_SYSCALL_H_

#include <stdint.h>
#include <sos.h>
#include <serial/serial.h>
#include "process.h"
#include "addrspace.h"

typedef struct iovec {
    sos_vaddr start;
    size_t sz;
    struct iovec *next;
} iovec_t;

size_t sys_print(size_t num_args);

int sos__sys_open(client_vaddr path, fmode_t mode, int *ret);

int sos__sys_read(int file, client_vaddr buf, size_t nbyte, int *ret);

int sos__sys_write(int file, client_vaddr buf, size_t nbyte, int *ret);

#endif
