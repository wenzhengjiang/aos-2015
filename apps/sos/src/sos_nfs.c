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

static inline unsigned CONST umin(unsigned a, unsigned b)
{
    return (a < b) ? a : b;
}

/* FILE OPENING */

static void
sos_nfs_create_callback(uintptr_t token, enum nfs_stat status, fhandle_t *fh,
                        fattr_t *fattr) {
    set_current_process(token);
    sos_proc_t *proc = process_lookup(token);
    int fd = proc->cont.fd;
    if (status != NFS_OK) {
        fd_free(proc->fd_table, fd);
        syscall_end_continuation(proc, SOS_NFS_ERR, false);
        return;
    }

    of_entry_t *of = fd_lookup(proc, fd);
    of->fhandle = malloc(sizeof(fhandle_t));
    if (of->fhandle == NULL) {
        fd_free(proc->fd_table, fd);
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
    set_current_process(token);
    printf("Entering nfs_open callback: %d\n", (int)token);
    sos_proc_t *proc = process_lookup(token);
    assert(proc);
    int fd = proc->cont.fd;
    printf("fd found: %d\n", fd);
    of_entry_t *of = fd_lookup(proc, fd);
    printf("of found %x\n", (unsigned)of);
    assert(of);
    printf("status: %d %s\n", status, proc->cont.path);
    if (status == NFSERR_NOENT && (of->mode & FM_WRITE)) {
        printf("Open calling for creation.\n");
        // TODO: Implement time stamps
        uint32_t clock_upper = time_stamp() >> 32;
        uint32_t clock_lower = (time_stamp() << 32) >> 32;

        struct sattr default_attr = {.mode = 0x7,
                                     .uid = 0,
                                     .gid = 0,
                                     .size = 0,
                                     .atime = {clock_upper, clock_lower},
                                     .mtime = {clock_upper, clock_lower}};
        dprintf(2, "sos_nfs_open_callback %s %d\n", proc->cont.path, proc->cont.fd);
        nfs_create(&mnt_point, proc->cont.path, &default_attr, sos_nfs_create_callback, proc->pid);
        return;
    } else if (status != NFS_OK) {
        // Clean up the preemptively created FD.
        fd_free(proc->fd_table, fd);
        syscall_end_continuation(proc, SOS_NFS_ERR, false);
        return;
    }
    printf("File already existsh on FS.\n");

    of->fhandle = (fhandle_t*)malloc(sizeof(fhandle_t));
    *(of->fhandle) = *fh;
    assert(proc);
    syscall_end_continuation(proc, fd, true);
    printf("Finishing nfs_open callback\n");
}

int sos_nfs_open(const char* filename, fmode_t mode) {
    sos_proc_t *proc = current_process();
    pid_t pid = current_process()->pid;
    dprintf(2, "sos_nfs_open %s %d\n", filename, proc->cont.fd);
    return nfs_lookup(&mnt_point, filename, sos_nfs_open_callback,
                      (unsigned)pid);
}

/* READ FILE */

static void
sos_nfs_read_callback(uintptr_t token, enum nfs_stat status,
                      fattr_t *fattr, int count, void* data) {
    (void)fattr;
    set_current_process(token);
    sos_proc_t *proc;
    int fd;
    proc = process_lookup((int)token);
    fd = proc->cont.fd;

    if (count == 0) {
        syscall_end_continuation(proc, proc->cont.counter, true);
        return;
    }

    if (status != NFS_OK) {
        syscall_end_continuation(proc, SOS_NFS_ERR, false);
        return;
    }
    proc->cont.counter += count;
    of_entry_t *of = fd_lookup(proc, fd);
    of->offset += (unsigned)count;

    sos_vaddr dst = as_lookup_sos_vaddr(proc->vspace, proc->cont.iov->vstart);
    assert(dst);

    memcpy((char*)dst, data, (size_t)count);
    dprintf(2, "READ %d bytes to %08x, now at offset: %u\n", count, proc->cont.iov->vstart, of->offset);

    iovec_t *iov = proc->cont.iov;
    if (proc->cont.iov->sz == (size_t)count) {
        proc->cont.iov = iov->next;
        free(iov);
        iov = proc->cont.iov;
    } else {
        iov->vstart += (size_t)count;
        iov->sz -= (size_t)count;
    }

    if (iov == NULL) {
        syscall_end_continuation(proc, proc->cont.counter, true);
        return;
    }
    add_callback_pid(token);
}

// TODO: Tidy up these params
int sos_nfs_read(iovec_t* vec, int fd, int count) {
    sos_proc_t *proc = current_process();
    pid_t pid = proc->pid;
    of_entry_t *of = fd_lookup(current_process(), fd);

    assert(proc->cont.iov);
    iov_ensure_loaded(proc->cont.iov);

    dprintf(2, "READING up to %d bytes to %08x, now at offset: %u\n",proc->cont.iov->sz, proc->cont.iov->vstart, of->offset);
    return nfs_read(of->fhandle, (int)of->offset, (int)proc->cont.iov->sz, sos_nfs_read_callback, (unsigned)pid);
}

/* WRITE FILE */

static void
nfs_write_callback(uintptr_t token, enum nfs_stat status, fattr_t *fattr, int count) {

    set_current_process(token);
    sos_proc_t *proc = proc = process_lookup(token);
    int fd = proc->cont.fd;
    if (status != NFS_OK) {
        syscall_end_continuation(proc, SOS_NFS_ERR, false);
        return;
    }

    proc->cont.counter += count;
    of_entry_t *of = fd_lookup(proc, fd);
    of->offset += (unsigned)count;

    iovec_t *iov = proc->cont.iov;
    if (proc->cont.iov->sz == (size_t)count) {
        proc->cont.iov = iov->next;
        free(iov);
        iov = proc->cont.iov;
    } else {
        iov->vstart += (size_t)count;
        iov->sz -= (size_t)count;
    }
    if (iov == NULL) {
        syscall_end_continuation(proc, proc->cont.counter, true);
        return;
    }
    add_callback_pid(token);
}

int sos_nfs_write(iovec_t* iov, int fd, int count) {
    of_entry_t *of = fd_lookup(current_process(), fd);
    dprintf(2, "[WRITE] Using %x for fd %d\n", of, fd);
    sos_proc_t *proc = current_process();
    pid_t pid = proc->pid;

    assert(proc->cont.iov);
    iov_ensure_loaded(proc->cont.iov);

    sos_vaddr src = as_lookup_sos_vaddr(proc->vspace, iov->vstart);
    assert(src);
    dprintf(2, "Writing to offset: %u %d (%d)bytes\n", of->offset, count, iov->sz);
    int err = nfs_write(of->fhandle, of->offset, iov->sz,
                        (const void*)src, nfs_write_callback,
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
    set_current_process(token);
    sos_proc_t* proc = process_lookup(token);
    if (status != NFS_OK) {
        syscall_end_continuation(proc, SOS_NFS_ERR, false);
        ERR("NFS failed\n");
        return;
    }
    sos_stat_t sos_attr;
    sos_attr.st_type = fattr->type;
    sos_attr.st_fmode = (int)fattr->mode;
    sos_attr.st_size = fattr->size;
    sos_attr.st_ctime = (long)fattr->ctime.seconds;
    sos_attr.st_atime = (long)fattr->atime.seconds;
    ipc_write(1, (char*)&sos_attr, sizeof(sos_stat_t));
    proc->cont.reply_length = 2 + ((sizeof(sos_stat_t) + sizeof(seL4_Word) - 1) >> 2);
    prstat(sos_attr);
    syscall_end_continuation(proc, status, true);
}

static void sos_nfs_lookup_for_attr(uintptr_t token, enum nfs_stat status,
                                    fhandle_t* fh, fattr_t* fattr) {
    pid_t pid = current_process()->pid;
    sos_proc_t* proc = process_lookup(token);
    if (status != NFS_OK) {
        syscall_end_continuation(proc, SOS_NFS_ERR, false);
        ERR("Did not find file\n");
        return;
    }
    nfs_getattr(fh, sos_nfs_getattr_callback, (unsigned)pid);
}

int sos_nfs_getattr(void) {
    sos_proc_t *proc = current_process();
    pid_t pid = proc->pid;
    // TODO: Handle cases where this returns non-zero in syscall.c. i.e.,
    // reply to the client with failure.
    int err = nfs_lookup(&mnt_point, current_process()->cont.path, sos_nfs_lookup_for_attr,
                       (unsigned)pid);
    if (err) {
        ERR("NFS stat said: %d\n", err);
    }
    return err;
}

/* READ DIRECTORY */

static void
nfs_readdir_callback(uintptr_t token, enum nfs_stat status, int num_files,
                     char* file_names[], nfscookie_t nfscookie) {
    set_current_process(token);
    sos_proc_t *proc = process_lookup(token);
    if (status != NFS_OK) {
        syscall_end_continuation(proc, SOS_NFS_ERR, false);
        return;
    }
    if (proc->cont.position_arg <= 0) {
        syscall_end_continuation(proc, SOS_NFS_ERR, false);
        return;
    }
    dprintf(2, "readir_callback:count=%d,target=%d,nfiles=%d\n", proc->cont.counter, proc->cont.position_arg, num_files);
    if (proc->cont.position_arg <= proc->cont.counter + num_files) {
        char *file = file_names[proc->cont.position_arg - proc->cont.counter - 1];
        size_t str_len = umin(strlen(file) + 1, proc->cont.length_arg);
        ipc_write(1, file, str_len);
        proc->cont.reply_length = 2 + ((str_len + sizeof(seL4_Word) - 1) >> 2);
        syscall_end_continuation(proc, strlen(file) + 1, true);
        return;
    }
    proc->cont.counter += num_files;

    if (nfscookie == 0) {
        proc->cont.reply_length = 2;
        seL4_SetMR(1, 0);
        syscall_end_continuation(proc, 0, true);
        return;
    }
    proc->cont.cookie = nfscookie;
    add_callback_pid(token);
}

int sos_nfs_readdir(void) {
    pid_t pid = current_process()->pid;
    if (current_process()->cont.length_arg == 0) {
        return 1;
    }
    int err = nfs_readdir(&mnt_point, current_process()->cont.cookie, nfs_readdir_callback, (unsigned)pid);
    if (err < 0) {
        return -err;
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
