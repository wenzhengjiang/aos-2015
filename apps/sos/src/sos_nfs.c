#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <clock/clock.h>

#include <nfs/nfs.h>
#include <clock/clock.h>
#include "network.h"
#include "file.h"
#include "process.h"
#include "sos_nfs.h"
#include "syscall.h"
#include "addrspace.h"

#define verbose 0
#include <log/debug.h>
#include <log/panic.h>

#define SOS_NFS_ERR (-1)

io_device_t nfs_io = {
    .open = sos_nfs_open,
    .close = NULL,
    .read = sos_nfs_read,
    .write = sos_nfs_write,
    .stat = sos_nfs_getattr,
    .getdirent = sos_nfs_readdir
};

static timestamp_t prevt;
/* FILE OPENING */

extern int pkg_size;
extern int pkg_num;
extern bool nfs_pkg;

static void
sos_nfs_create_callback(uintptr_t token, enum nfs_stat status, fhandle_t *fh,
                        fattr_t *fattr) {
    sos_proc_t *proc = process_lookup(token);
    int fd = proc->cont.fd;
    if (status != NFS_OK) {

        fd_free(proc, fd);
        syscall_end_continuation(proc, SOS_NFS_ERR, false);
        return;
    }

    of_entry_t *of = fd_lookup(proc, fd);
    of->fhandle = malloc(sizeof(fhandle_t));
    if (of->fhandle == NULL) {
        fd_free(proc, fd);
        syscall_end_continuation(proc, -1, false);
        return;
    }
    *(of->fhandle) = *fh;
    dprintf(2, "sos_nfs_create_callback %d\n", proc->cont.fd);
    syscall_end_continuation(proc, fd, true);
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
        dprintf(2, "sos_nfs_open_callback %s %d\n", proc->cont.filename, proc->cont.fd);
        nfs_create(&mnt_point, proc->cont.filename, &default_attr, sos_nfs_create_callback, proc->pid);
        return;
    } else if (status != NFS_OK) {
        // Clean up the preemptively created FD.
        fd_free(proc, fd);
        syscall_end_continuation(proc, SOS_NFS_ERR, false);
        return;
    }

    of->fhandle = (fhandle_t*)malloc(sizeof(fhandle_t));
    *(of->fhandle) = *fh;

    syscall_end_continuation(proc, fd, true);
}

int sos_nfs_open(const char* filename, fmode_t mode) {
    sos_proc_t *proc = current_process();
    int fd = fd_create(proc->fd_table, NULL,  &nfs_io, mode);
    proc->cont.fd = fd;
    proc->cont.filename = filename;
    pid_t pid = current_process()->pid;
    dprintf(2, "sos_nfs_open %s %d\n", filename, proc->cont.fd);
    return nfs_lookup(&mnt_point, filename, sos_nfs_open_callback,
                      (unsigned)pid);
}

/* READ FILE */

static void
sos_nfs_read_callback(uintptr_t token, enum nfs_stat status,
                      fattr_t *fattr, int count, void* data) {
    if (count > pkg_size) pkg_size = count;
    (void)fattr;
    prevt = time_stamp();
    sos_proc_t *proc;
    int fd;
    proc = process_lookup((int)token);
    fd = proc->cont.fd;

    if (status != NFS_OK) {
        syscall_end_continuation(proc, SOS_NFS_ERR, false);
        return;
    }
    proc->cont.counter += count;
     of_entry_t *of = fd_lookup(proc, fd);
    of->offset += (unsigned)count;

    iovec_t *iov = proc->cont.iov;
    if ((int)iov->sz != count) {
        dprintf(-1, "iovsz = %d, count = %d\n", iov->sz, count);
    }
    memcpy((char*)iov->start, data, (size_t)count);
    proc->cont.iov = iov->next;
    free(iov);
    if (proc->cont.iov == NULL) {
        syscall_end_continuation(proc, proc->cont.counter, true);
        return ;
    }
    dprintf(2, "read %d bytes to %08x, now at offset: %u\n",proc->cont.iov->sz,proc->cont.iov->start, of->offset);
    int err = nfs_read(of->fhandle, (int)of->offset, (int)proc->cont.iov->sz,
                       sos_nfs_read_callback, (unsigned)proc->pid);
    if (err != RPC_OK) {
        syscall_end_continuation(proc, SOS_NFS_ERR, false);
        return;
    }
}

int sos_nfs_read(iovec_t* vec, int fd, int count) {
    sos_proc_t *proc = current_process();
    pid_t pid = proc->pid;
    proc->cont.fd = fd;
    proc->cont.iov = vec;
    of_entry_t *of = fd_lookup(current_process(), fd);
    dprintf(2, "read %d bytes to %08x, now at offset: %u\n",proc->cont.iov->sz,proc->cont.iov->start, of->offset);
    prevt = time_stamp();
    pkg_size = pkg_num = 0;
    nfs_pkg = true;
    return nfs_read(of->fhandle, of->offset, proc->cont.iov->sz, sos_nfs_read_callback, (unsigned)pid);
}

/* WRITE FILE */

static void
nfs_write_callback(uintptr_t token, enum nfs_stat status, fattr_t *fattr, int count) {
    if (count > pkg_size) pkg_size = count;

    dprintf(5, "write_callback %d %llu\n",  count, time_stamp()-prevt);
    prevt = time_stamp();
    sos_proc_t *proc = proc = process_lookup(token);
    int fd = proc->cont.fd;

    if (status != NFS_OK) {
        syscall_end_continuation(proc, SOS_NFS_ERR, false);
        return;
    }

    proc->cont.counter += count;
    of_entry_t *of = fd_lookup(proc, fd);
    dprintf(1, "[WRITE] for %d bytes\n", count);
    of->offset += (unsigned)count;

    iovec_t *iov = proc->cont.iov;
    if (proc->cont.iov->sz == count) {
        proc->cont.iov = iov->next;
        free(iov);
    } else {
        iov->start += count;
        iov->sz -= count;
    }
    if (proc->cont.iov == NULL) {
        dprintf(2, "wrote %d bytes.  now at offset: %u\n", count, of->offset);
        syscall_end_continuation(proc, proc->cont.counter, true);
        return;
    } else {
        iov = proc->cont.iov;
        if (nfs_write(of->fhandle, of->offset, iov->sz, (const void*)iov->start, nfs_write_callback, (unsigned)proc->pid) != RPC_OK) {
            syscall_end_continuation(proc, SOS_NFS_ERR, false);
            return;
        }
    }
}

int sos_nfs_write(iovec_t* iov, int fd, int count) {
    of_entry_t *of = fd_lookup(current_process(), fd);
    dprintf(2, "[WRITE] Using %x for fd %d\n", of, fd);
    sos_proc_t *proc = current_process();
    pid_t pid = proc->pid;
    proc->cont.iov = iov;
    proc->cont.fd = fd;
    dprintf(2, "Writing to offset: %u %d (%d)bytes\n", of->offset, count, iov->sz);
    prevt = time_stamp();
    pkg_size = pkg_num = 0;
    nfs_pkg = true;
    int err = nfs_write(of->fhandle, of->offset, iov->sz,
                        (const void*)iov->start, nfs_write_callback,
                        (unsigned)pid);
    dprintf(3, "Result from nfs write: %d\n", err);
    return 0;
}

/* GET FILE ATTRIBUTES */

static void prstat(sos_stat_t sbuf) {
    /* print out stat buf */
    dprintf(2, "%c%c%c%c 0x%06x 0x%lx 0x%06lx\n",
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
        syscall_end_continuation(proc, SOS_NFS_ERR, false);
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
    syscall_end_continuation(proc, status, true);
}

static void sos_nfs_lookup_for_attr(uintptr_t token, enum nfs_stat status,
                                    fhandle_t* fh, fattr_t* fattr) {
    pid_t pid = current_process()->pid;
    sos_proc_t* proc = process_lookup(token);
    if (status != NFS_OK) {
        syscall_end_continuation(proc, SOS_NFS_ERR, false);
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
        syscall_end_continuation(proc, SOS_NFS_ERR, false);
        return;
    }
    if (proc->cont.target <= 0) {
        syscall_end_continuation(proc, SOS_NFS_ERR, false);
        return;
    }
    dprintf(2, "readir_callback:count=%d,target=%d,nfiles=%d\n", proc->cont.counter, proc->cont.target, num_files);
    if (proc->cont.target <= proc->cont.counter + num_files) {
        char *file = file_names[proc->cont.target - proc->cont.counter - 1];
        iovec_t *iov = proc->cont.iov;
        iov_read(iov, file, strlen(file)+1);
        syscall_end_continuation(proc, strlen(file)+1, true);
        return;
    }
    proc->cont.counter += num_files;
    if (nfscookie == 0) {
        syscall_end_continuation(proc, 0, true);
        return;
    }
    if (nfs_readdir(&mnt_point, nfscookie, nfs_readdir_callback,
                    (unsigned)proc->pid) != RPC_OK) {;
        syscall_end_continuation(proc, SOS_NFS_ERR, false);
        return;
    }
    return;
}

int sos_nfs_readdir(int stop_index, iovec_t *iov) {
    sos_proc_t *proc = current_process();
    pid_t pid = current_process()->pid;
    proc->cont.target = stop_index + 1;
    proc->cont.iov = iov;
    int err = nfs_readdir(&mnt_point, 0, nfs_readdir_callback, (unsigned)pid);
    if (err < 0) {
        return err;
    }
    return 0;
}

int sos_nfs_init(const char* dir) {
    int err;
    err = register_tick_event(nfs_timeout);
    if (err) {
        return err;
    }
    return 0;
}