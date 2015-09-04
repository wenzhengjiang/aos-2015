#ifndef _SOS_FILE_H_
#define _SOS_FILE_H_

#include <sos.h>
#include <nfs/nfs.h>
#include "syscall.h"

#define MAX_FD (1023)

typedef struct open_file_entry {
    size_t offset;
    fmode_t mode;
    fhandle_t * nfs_handle;
    io_device_t * io;
} of_entry_t;

typedef of_entry_t** fd_table_t;

int fd_create(fd_table_t fdt, fhandle_t *handle, io_device_t* io, fmode_t mode);
int init_fd_table(void) ;

#endif
