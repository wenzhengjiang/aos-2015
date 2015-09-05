#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>

#include <nfs/nfs.h>
#include <clock/clock.h>
#include "network.h"
#include "file.h"
#include "process.h"
#include "sos_nfs.h"
#include "syscall.h"
#include "addrspace.h"

#define verbose 5
#include <log/debug.h>
#include <log/panic.h>

io_device_t nfs_io = {
    .open = sos_nfs_open,
    .close = NULL,
    .read = sos_nfs_read,
    .write = sos_nfs_write,
    .stat = sos_nfs_getattr,
    .getdirent = sos_nfs_readdir
};

/* FILE OPENING */

static void
sos_nfs_create_callback(uintptr_t token, enum nfs_stat status, fhandle_t *fh,
                        fattr_t *fattr) {
    sos_proc_t *proc;
    int fd;
    proc = process_lookup(token);
    fd = proc->cont.fd;
    if (status != NFS_OK) {
        // Clean up the preemptively created FD.  TODO: Consider whether we
        // should just be passing the error straight through, or should be
        // standardising them somehow
        fd_free(proc, fd);
        syscall_end_continuation(proc, -status);
        return;
    }

    of_entry_t *of = fd_lookup(proc, fd);
    of->fhandle = malloc(sizeof(fhandle_t));
    if (of->fhandle == NULL) {
        fd_free(proc, fd);
        syscall_end_continuation(proc, -1);
        return;
    }
    *(of->fhandle) = *fh;

    printf("sos_nfs_create_callback %d\n", proc->cont.fd);
    syscall_end_continuation(proc, fd);
}

static void
sos_nfs_open_callback(uintptr_t token, enum nfs_stat status,
                      fhandle_t* fh, fattr_t* fattr) {
    sos_proc_t *proc = process_lookup(token);
    int fd = proc->cont.fd;
    of_entry_t *of = fd_lookup(proc, fd);
    if (status == NFSERR_NOENT && (of->mode & FM_WRITE)) {
        // TODO: Implement time stamps
        uint32_t clock_upper = time_stamp() >> 32;
        uint32_t clock_lower = (time_stamp() << 32) >> 32;
        struct sattr default_attr = {.mode = 0x7,
                                     .uid = 0,
                                     .gid = 0,
                                     .size = 0,
                                     .atime = {clock_upper, clock_lower},
                                     .mtime = {clock_upper, clock_lower}};
        printf("sos_nfs_open_callback %s\n", proc->cont.filename);
        nfs_create(&mnt_point, proc->cont.filename, &default_attr,
                   sos_nfs_create_callback, proc->pid);
    } else if (status != NFS_OK) {
        // Clean up the preemptively created FD.
        fd_free(proc, fd);
        syscall_end_continuation(proc, -status);
        return;
    }

    of->fhandle = (fhandle_t*)malloc(sizeof(fhandle_t));
    *(of->fhandle) = *fh;

    syscall_end_continuation(proc, fd);
}

int sos_nfs_open(const char* filename, fmode_t mode) {
    sos_proc_t *proc = current_process();
    int fd = fd_create(proc->fd_table, NULL,  &nfs_io, mode);
    proc->cont.fd = fd;
    proc->cont.filename = filename;
    pid_t pid = current_process()->pid;
    printf("sos_nfs_open %s %d\n", filename, fd);
    return nfs_lookup(&mnt_point, filename, sos_nfs_open_callback,
                      (unsigned)pid);
}

/* READ FILE */

static void
sos_nfs_read_callback(uintptr_t token, enum nfs_stat status,
                      fattr_t *fattr, int count, void* data) {
    sos_proc_t *proc;
    int fd;
    proc = process_lookup(token);
    fd = proc->cont.fd;

    if (status != NFS_OK) {
        syscall_end_continuation(proc, -status);
        return;
    }
    iov_read(proc->cont.iov, data, count);
    of_entry_t *of = fd_lookup(proc, fd);
    of->offset += count; 
    syscall_end_continuation(proc, count);
}

int sos_nfs_read(iovec_t* vec, int fd, int count) {
    sos_proc_t *proc = current_process();
    pid_t pid = proc->pid;
    // TODO: Check FD is open with mode allowing read (but don't do it here!)
    proc->cont.fd = fd;
    proc->cont.iov = vec;
    of_entry_t *of = fd_lookup(current_process(), fd);
    return nfs_read(of->fhandle, of->offset, count, sos_nfs_read_callback,
                    (unsigned)pid);
}

/* WRITE FILE */

static void
nfs_write_callback(uintptr_t token, enum nfs_stat status, fattr_t *fattr, int count) {
    sos_proc_t *proc;
    int fd;
    of_entry_t *of;
    proc = process_lookup(token);
    fd = proc->cont.fd;

    if (status != NFS_OK) {
        syscall_end_continuation(proc, -status);
        return;
    }
    iovec_t *iov = proc->cont.iov;
    if (iov == NULL) {
        proc->cont.counter += count;
        syscall_end_continuation(proc, proc->cont.counter);
    } else {
        printf("nfs_write_callback: %d\n", count);
        proc->cont.iov = iov->next;
        free(iov);
        iov = proc->cont.iov;
        of = fd_lookup(proc, fd);
        of->offset += count;
        if (nfs_write(of->fhandle, of->offset, iov->sz, (const void*)iov->start,
                      nfs_write_callback,
                      (unsigned)proc->pid) != RPC_OK) {
            syscall_end_continuation(proc, -status);
        }
    }
}

int sos_nfs_write(iovec_t* iov, int fd, int count) {
    of_entry_t *of = fd_lookup(current_process(), fd);
    sos_proc_t *proc = current_process();
    pid_t pid = proc->pid;
    proc->cont.iov = iov;
    return nfs_write(of->fhandle, of->offset, iov->sz,
                     (const void*)iov->start, nfs_write_callback,
                     (unsigned)pid);
}

/* GET FILE ATTRIBUTES */

static void prstat(sos_stat_t sbuf) {
    /* print out stat buf */
    printf("%c%c%c%c 0x%06x 0x%lx 0x%06lx\n",
            sbuf.st_type == ST_SPECIAL ? 's' : '-',
            sbuf.st_fmode & FM_READ ? 'r' : '-',
            sbuf.st_fmode & FM_WRITE ? 'w' : '-',
            sbuf.st_fmode & FM_EXEC ? 'x' : '-', sbuf.st_size, sbuf.st_ctime,
            sbuf.st_atime);
}
static void
sos_nfs_getattr_callback(uintptr_t token, enum nfs_stat status, fattr_t *fattr) {
    sos_proc_t* proc = process_lookup(token);
    if (status != NFS_OK) {
        syscall_end_continuation(proc, -status);
        return;
    }
    sos_stat_t sos_attr;
    sos_attr.st_type = fattr->type;
    sos_attr.st_fmode = (int)fattr->mode;
    sos_attr.st_size = fattr->size;
    sos_attr.st_ctime = (long)fattr->ctime.seconds;
    sos_attr.st_atime = (long)fattr->atime.seconds;
    iov_read(proc->cont.iov, (char*)(&sos_attr), sizeof(sos_stat_t));
    prstat(sos_attr);
    syscall_end_continuation(proc, status);
}

static void sos_nfs_lookup_for_attr(uintptr_t token, enum nfs_stat status,
                                    fhandle_t* fh, fattr_t* fattr) {
    pid_t pid = current_process()->pid;
    sos_proc_t* proc = process_lookup(token);
    if (status != NFS_OK) {
        syscall_end_continuation(proc, -status);
        return;
    }
    nfs_getattr(fh, sos_nfs_getattr_callback, (unsigned)pid);
}

int sos_nfs_getattr(char* filename, iovec_t* iov) {
    sos_proc_t *proc =current_process();
    pid_t pid = proc->pid;
    proc->cont.iov = iov;

    // TODO: Handle cases where this returns non-zero in syscall.c. i.e.,
    // reply to the client with failure.
    return nfs_lookup(&mnt_point, filename, sos_nfs_lookup_for_attr,
                      (unsigned)pid);
}

/* READ DIRECTORY */

static void
nfs_readdir_callback(uintptr_t token, enum nfs_stat status, int num_files,
                     char* file_names[], nfscookie_t nfscookie) {
    sos_proc_t *proc = process_lookup(token);
    if (status != NFS_OK) {
        syscall_end_continuation(proc, -status);
        return;
    }
    if (proc->cont.target == 0) return ; //TODO why target could be zero ?
    printf("readir_callback:count=%d,target=%d,nfiles=%d\n", proc->cont.counter, proc->cont.target, num_files);
    assert(proc->cont.counter < proc->cont.target);

    if (proc->cont.target <= proc->cont.counter + num_files) {
        char *file = file_names[proc->cont.target-proc->cont.counter-1];
        iovec_t *iov = proc->cont.iov;
        iov_read(iov, file, strlen(file)+1);
        syscall_end_continuation(proc, strlen(file)+1);
        return;
    }
    proc->cont.counter += num_files;
    if (nfscookie != 0) {
        if (nfs_readdir(&mnt_point, nfscookie, nfs_readdir_callback,
                        (unsigned)proc->pid) != RPC_OK) {;
            syscall_end_continuation(proc, -status);
        }
    }
    syscall_end_continuation(proc, 0);
    return;
}

int sos_nfs_readdir(int stop_index, iovec_t *iov) {
    sos_proc_t *proc = current_process();
    pid_t pid = current_process()->pid;
    proc->cont.target = stop_index+1;
    proc->cont.iov = iov;
    return nfs_readdir(&mnt_point, 0, nfs_readdir_callback,
                       (unsigned)pid);
}

int sos_nfs_init(const char* dir) {
    int err;
    err = register_tick_event(nfs_timeout);
    if (err) {
        return err;
    }
    return 0;
}
