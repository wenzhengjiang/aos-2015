#ifndef _SOS_SYSCALL_H_
#define _SOS_SYSCALL_H_

#include <sos.h>
#include <stdint.h>
#include <serial/serial.h>
#include "addrspace.h"

typedef struct iovec {
    sos_vaddr start;
    size_t sz;
    struct iovec *next;
} iovec_t;


typedef struct io_device {
    int (*open)(const char*, fmode_t);
    int (*close)(void);
    int (*read)(iovec_t*, int fd, int count);
    int (*write)(iovec_t*, int fd, int count);
    int (*stat)(char*, iovec_t*);
    int (*getdirent)(int, iovec_t*);
} io_device_t;


size_t sys_print(size_t num_args);

int sos__sys_open(const char *path, fmode_t mode);

int sos__sys_read(int file, client_vaddr buf, size_t nbyte);

int sos__sys_write(int file, client_vaddr buf, size_t nbyte);

int sos__sys_stat(char *path, client_vaddr buf) ;

int sos__sys_getdirent(int pos, client_vaddr name, size_t nbyte);

void ipc_read(int start, char *buf);

int iov_read(iovec_t *, char* buf, int count);

void iov_free(iovec_t *);

#endif
