#ifndef _SOS_NFS_H_
#define _SOS_NFS_H_

#include "syscall.h"

int sos_nfs_open(char* filename, fmode_t mode);

int sos_nfs_read(iovec_t* vec, int fd, int count);

int sos_nfs_write(iovec_t* iov, int fd);

int sos_nfs_getattr(iovec_t* iov, int fd);

int sos_nfs_readdir(iovec_t iov, int fd, int stop_index);

int sos_nfs_init(const char* dir);

#endif
