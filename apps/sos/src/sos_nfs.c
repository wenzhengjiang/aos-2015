#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include <nfs/nfs.h>
#include <clock/clock.h>
#include "io_device.h"
#include "file.h"
#include "process.h"
#include "sos_nfs.h"

io_device_t nfs_io = {
    .open = sos_nfs_open,
    .close = NULL,
    .read = sos_nfs_read,
    .write = sos_nfs_write,
    .getattr = sos_nfs_getattr,
    .readdir = sos_nfs_readdir
};

fhandle_t mnt_point = NULL;

/* FILE OPENING */

static void
sos_nfs_open_callback(uintptr_t token, enum nfs_stat status,
                  fhandle_t* fh, fattr_t* fattr) {
    sos_proc_t *proc;
    int fd;
    open_file_t *of;
    proc = process_lookup(token);
    assert(proc->continuation);
    fd = proc->continuation->fd;

    if (status != NFS_OK) {
        // Clean up the preemptively created FD.  TODO: Consider whether we
        // should just be passing the error straight through, or should be
        // standardising them somehow
        fd_free(current_process, fd);
        syscall_finalise(-status);
        return;
    }

    of = proc_fd(proc, fd);
    of->fhandle = fh;

    // TODO: Should we do this?
    syscall_finalise(fd);
}

int sos_nfs_open(char* filename, fmode_t mode) {
    assert(current_process()->continuation);
    continuation_create();
    int fd = fd_create(current_process(), NULL, 0, nfs_io, mode);
    current_process()->continuation->fd = fd;
    pid_t pid = current_process()->pid;
    return nfs_lookup(mnt_point, filename, sos_nfs_open_callback, pid);
}


/* READ FILE */

static void
sos_nfs_read_callback(uintptr_t token, enum nfs_stat status,
                      fattr_t *fattr, int count, void* data) {
    sos_proc_t *proc;
    int fd;
    open_file_t *of;
    proc = process_lookup(token);
    assert(proc->continuation);
    fd = proc->continuation->fd;

    if (status != NFS_OK) {
        syscall_finalise(-status);
    } else {
        iovec_read(proc->continuation->iovec, data, count);

        // TODO: Should we do this?
        syscall_finalise(fd);
    }
}

int sos_nfs_read(iovec_t* vec, int fd, int count) {
    pid_t pid = curproc()->pid;
    // TODO: Check FD is open with mode allowing read (but don't do it here!)
    cont_t* cont = continuation_create();
    current_process()->continuation->fd = fd;
    current_process()->continuation->iovec = vec;
    pid_t pid = current_process()->pid;
    open_file_t *of = fd_lookup(current_process(), fd);
    return nfs_read(of->fhandle, of->offset, count,
                    sos_nfs_read_callback, pid);
}

/* WRITE FILE */

static void
nfs_write_callback(uintptr_t token, enum nfs_stat status,
                   fattr_t *fattr, int count) {
    sos_proc_t *proc;
    int fd;
    open_file_t *of;
    proc = process_lookup(token);
    assert(proc->continuation);
    fd = proc->continuation->fd;

    if (status != NFS_OK) {
        syscall_finalise(-status);
        return;
    }
    iovec_t *iov = proc->continuation->iovec;
    if (iov == NULL) {
        proc->continuation->counter += count;
        syscall_finalise(proc->continuation->counter);
    } else {
        of = fd_lookup(proc, fd);
        of->offset += count;
        nfs_write(fh, of->offset, iov->sz, iov->start,
                  nfs_write_callback, pid);
    }
}

int sos_nfs_write(iovec_t* iov, int fd) {
    open_file_t *of = proc_fd(curproc(), fd);
    pid_t pid = curproc()->pid;
    continuation_create();
    curproc->continuation->iovec = iov;
    return nfs_write(of->fhandle, of->start, of->sz, nfs_write_callback, pid);
}

/* GET FILE ATTRIBUTES */

static void
nfs_getattr_callback(uintptr_t token, enum nfs_stat status, fattr_t *fattr) {
    sos_process_t* proc = process_lookup(token);
    continuation_create();
    if (status != NFS_OK) {
        syscall_finalise(-status);
        return;
    }
    sos_stat_t sos_attr;
    sos_attr.st_type = fattr->type;
    sos_attr.st_fmode = fattr->mode;
    sos_attr.st_size = fattr->size;
    sos_attr.st_ctime = fattr->ctime;
    sos_attr.st_atime = fattr->atime;
    iovec_read(proc->continuation->iovec, &sos_attr, sizeof(sos_stat_t));
    syscall_finalise(status);
}

int sos_nfs_getattr(int fd, iovec_t* iov) {
    open_file_t *of = proc_fd(curproc(), fd);
    pid_t pid = curproc()->pid;
    continuation_create();
    proc->continuation->iovec = iov;
    return nfs_getattr(of->fhandle, sos_nfs_getattr_callback, pid);
}

/* READ DIRECTORY */

static void
nfs_readdir_callback(uintptr_t token, enum nfs_stat status, int num_files,
                     char* file_names[], nfscookie_t nfscookie) {
    sos_process_t *proc = process_lookup(token);
    if (status != NFS_OK) {
        syscall_finalise(-status);
        return;
    }
    if (proc->continuation->target >= 0) {
        assert(proc->continuation->counter < proc->contination->target);
    }
    if (proc->continuation->target < proc->contination->counter + num_files) {
        char *file = file_names[proc->continuation->target - proc->continuation->counter];
        iovec_t *iov = proc->continuation->iovec;
        iovec_read(iov, file, strlen(file));
        syscall_finalise(status);
        return;
    }
    proc->continuation->counter += num_files;
    if (nfs_cookie != 0) {
        nfs_readdir(mnt_point, nfscookie, nfs_readdir_callback, pid);
    }
    return;
}

int sos_nfs_readdir(iovec_t iov, int fd, int stop_index) {
    open_file_t *of = proc_fd(curproc(), fd);
    pid_t pid = curproc()->pid;
    continuation_create();
    proc->continuation->target = stop_index;
    proc->continuation->iovec = iov;
    return nfs_readdir(mnt_point, 0, nfs_readdir_callback, pid);
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
