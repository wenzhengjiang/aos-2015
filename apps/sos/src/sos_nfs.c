#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include <nfs/nfs.h>
#include <clock/clock.h>
#include "io_device.h"
#include "file.h"
#include "process.h"
#include "sos_nfs.h"
#include "syscall.h"

io_device_t nfs_io = {
    .open = sos_nfs_open,
    .close = NULL,
    .read = sos_nfs_read,
    .write = sos_nfs_write,
    .getattr = sos_nfs_getattr,
    .readdir = sos_nfs_readdir
};

fhandle_t mnt_point = {{0}};

// TODO: REMOVE THESE!!  Here for aiding visibility of implementation problems
// only
typedef int pid_t;
typedef struct open_file {
    fhandle_t* fhandle;
    int offset;
    io_device_t iodevice;
} open_file_t;
typedef struct contination {
    int fd;
    seL4_CPtr reply_cap;
    int counter;
    int target;
    iovec_t* iovec;
}cont_t;

/* FILE OPENING */

static void
sos_nfs_open_callback(uintptr_t token, enum nfs_stat status,
                  fhandle_t* fh, fattr_t* fattr) {
    sos_proc_t *proc;
    int fd;
    open_file_t *of;
    cont_t *cont;
    proc = process_lookup(token);
    cont = proc->continuation;
    assert(cont);
    fd = cont->fd;
    if (status != NFS_OK) {
        // Clean up the preemptively created FD.  TODO: Consider whether we
        // should just be passing the error straight through, or should be
        // standardising them somehow
        fd_free(current_process, fd);
        syscall_end_continuation(proc, -status);
        return;
    }

    of = proc_fd(proc, fd);
    of->fhandle = fh;

    syscall_end_continuation(proc, fd);
}

int sos_nfs_open(char* filename, fmode_t mode) {
    cont_t* cont = continuation_create();
    assert(cont);
    int fd = fd_create(current_process(), NULL, 0, nfs_io, mode);
    cont->fd = fd;
    pid_t pid = current_process()->pid;
    return nfs_lookup(&mnt_point, filename, sos_nfs_open_callback,
                      (unsigned)pid);
}


/* READ FILE */

static void
sos_nfs_read_callback(uintptr_t token, enum nfs_stat status,
                      fattr_t *fattr, int count, void* data) {
    sos_proc_t *proc;
    int fd;
    open_file_t *of;
    cont_t* cont = contination_create();
    assert(cont);
    proc = process_lookup(token);
    fd = cont->fd;

    if (status != NFS_OK) {
        syscall_end_continuation(proc, -status);
        return;
    }
    iovec_read(cont->iovec, data, count);
    syscall_end_continuation(proc, fd);
}

int sos_nfs_read(iovec_t* vec, int fd, int count) {
    pid_t pid = current_process()->pid;
    // TODO: Check FD is open with mode allowing read (but don't do it here!)
    cont_t* cont = continuation_create();
    cont->fd = fd;
    cont->iovec = vec;
    open_file_t *of = fd_lookup(current_process(), fd);
    return nfs_read(of->fhandle, of->offset, count, sos_nfs_read_callback,
                    (unsigned)pid);
}

/* WRITE FILE */

static void
nfs_write_callback(uintptr_t token, enum nfs_stat status,
                   fattr_t *fattr, int count) {
    sos_proc_t *proc;
    int fd;
    open_file_t *of;
    proc = process_lookup(token);
    cont_t *cont = proc->continuation;
    assert(cont);
    fd = cont->fd;

    if (status != NFS_OK) {
        syscall_end_continuation(proc, -status);
        return;
    }
    iovec_t *iov = cont->iovec;
    if (iov == NULL) {
        cont->counter += count;
        syscall_end_continuation(proc, cont->counter);
    } else {
        cont->iovec = iov->next;
        free(iov);
        iov = cont->iovec;
        of = fd_lookup(proc, fd);
        of->offset += count;
        if (nfs_write(of->fhandle, of->offset, iov->sz, iov->start,
                  nfs_write_callback,
                      (unsigned)proc->pid) != RPC_OK) {
            syscall_end_continuation(proc, -status);
        }
    }
}

int sos_nfs_write(iovec_t* iov, int fd, int count) {
    open_file_t *of = proc_fd(current_process(), fd);
    pid_t pid = current_process()->pid;
    cont_t *cont = continuation_create();
    cont->iovec = iov;
    return nfs_write(of->fhandle, of->offset, iov->sz,
                     iov->start, nfs_write_callback,
                     (unsigned)pid);
}

/* GET FILE ATTRIBUTES */

static void
sos_nfs_getattr_callback(uintptr_t token, enum nfs_stat status, fattr_t *fattr) {
    sos_process_t* proc = process_lookup(token);
    if (status != NFS_OK) {
        syscall_end_continuation(proc, -status);
        return;
    }
    sos_stat_t sos_attr;
    cont_t *cont = proc->continuation;
    assert(cont);
    sos_attr.st_type = fattr->type;
    sos_attr.st_fmode = (int)fattr->mode;
    sos_attr.st_size = fattr->size;
    sos_attr.st_ctime = (long)fattr->ctime.seconds;
    sos_attr.st_atime = (long)fattr->atime.seconds;
    iovec_read(cont->iovec, &sos_attr, sizeof(sos_stat_t));
    syscall_end_continuation(proc, status);
}

int sos_nfs_getattr(iovec_t* iov, int fd) {
    open_file_t *of = proc_fd(current_process(), fd);
    pid_t pid = current_process()->pid;
    cont_t* cont = continuation_create();
    assert(cont);
    cont->iovec = iov;
    // TODO: Handle cases where this returns non-zero in syscall.c. i.e.,
    // reply to the client with failure.
    return nfs_getattr(of->fhandle, sos_nfs_getattr_callback,
                       (unsigned)pid);
}

/* READ DIRECTORY */

static void
nfs_readdir_callback(uintptr_t token, enum nfs_stat status, int num_files,
                     char* file_names[], nfscookie_t nfscookie) {
    sos_process_t *proc = process_lookup(token);
    cont_t* cont;
    if (status != NFS_OK) {
        syscall_end_continuation(proc, -status);
        return;
    }
    if (cont->target >= 0) {
        assert(cont->counter < cont->target);
    }
    if (cont->target < cont->counter + num_files) {
        char *file = file_names[cont->target - cont->counter];
        iovec_t *iov = cont->iovec;
        iovec_read(iov, file, strlen(file));
        syscall_end_continuation(proc, status);
        return;
    }
    cont->counter += num_files;
    if (nfscookie != 0) {
        if (nfs_readdir(&mnt_point, nfscookie, nfs_readdir_callback,
                        (unsigned)proc->pid) != RPC_OK) {;
            syscall_end_continuation(proc, -status);
        }
    }
    return;
}

int sos_nfs_readdir(iovec_t *iov, int fd, int stop_index) {
    open_file_t *of = proc_fd(current_process(), fd);
    pid_t pid = current_process()->pid;
    cont_t *cont = continuation_create();
    cont->target = stop_index;
    cont->iovec = iov;
    return nfs_readdir(&mnt_point, 0, nfs_readdir_callback,
                       (unsigned)pid);
}

int sos_nfs_init(const char* dir) {
    int err;
    err = nfs_mount(dir, &mnt_point);
    if (err) {
        return err;
    }
    register_tick_event(nfs_timeout);
    if (err) {
        return err;
    }
    return 0;
}
