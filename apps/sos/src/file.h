#ifndef _SOS_FILE_H_
#define _SOS_FILE_H_

#include <sos.h>
#include <nfs/nfs.h>
#include "addrspace.h"

#define MAX_FD (1023)

typedef struct io_device {
    int (*open)(const char*, fmode_t);
    int (*close)(int fd);
    int (*read)(iovec_t*, int fd, int count);
    int (*write)(iovec_t*, int fd, int count);
    int (*stat)(char*, iovec_t*);
    int (*getdirent)(int, iovec_t*);
} io_device_t;

typedef struct open_file_entry {
    size_t offset;
    fmode_t mode;
    fhandle_t* fhandle;
    io_device_t *io;
} of_entry_t;

typedef of_entry_t** fd_table_t;

int init_open_file_table(void);
int fd_create(fd_table_t fdt, fhandle_t* handle, io_device_t* io, fmode_t mode);
int init_fd_table(void);

#endif