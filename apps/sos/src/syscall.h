#ifndef _SOS_SYSCALL_H_
#define _SOS_SYSCALL_H_

#include <stdint.h>
#include "process.h"
#include "addrspace.h"

typedef struct iovec {
    sos_vaddr start;
    size_t n;
    struct iovec *next;
} iovec_t;

size_t sys_sys_print(size_t num_args);

int sos_sys_open(const char *path, fmode_t mode, int *ret);

int sos_sys_read(int file, char *buf, size_t nbyte, int *ret);

int sos_sys_write(int file, char *buf, size_t nbyte, int *ret);

#endif
