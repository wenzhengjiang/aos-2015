/**
 * @file syscall.c
 * @brief sos internal syscall implementations
 */

#include <sel4/sel4.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>
#include <string.h>
#include <errno.h>
#include <setjmp.h>
#include <serial/serial.h>
#include <cspace/cspace.h>
#include <limits.h>
#include "serial.h"
#include "frametable.h"
#include "page_replacement.h"
#include "process.h"
#include "file.h"
#include "addrspace.h"
#include "syscall.h"
#include <assert.h>
#include <sos.h>
#include <syscallno.h>
#include <clock/clock.h>

#define verbose 5
#include <log/debug.h>
#include <log/panic.h>

#define PRINT_MESSAGE_START (2)

extern io_device_t serial_io;
extern io_device_t nfs_io;
extern seL4_CPtr _sos_ipc_ep_cap;
/* Contains client whose syscall executions are ready to continue after callbacks get executed */
static pid_t ready_procs[MAX_PROCESS_NUM];
static int head = 0, tail = -1, nproc = 0;

extern jmp_buf ipc_event_env;

static inline unsigned CONST umin(unsigned a, unsigned b) {
    return (a < b) ? a : b;
}

static inline unsigned CONST umax(unsigned a, unsigned b) {
    return (a > b) ? a : b;
}

/**
 * @brief Add a client to ready client queue (resume a process)
 *
 */
void add_ready_proc(pid_t pid) {
    assert(pid);
    nproc++;
    tail = (tail+1) % MAX_PROCESS_NUM;
    ready_procs[tail] = pid;
    assert(nproc <= MAX_PROCESS_NUM);
    for (int i =  head; i < tail; i = (i+1)%MAX_PROCESS_NUM) {
        assert(ready_procs[i] != pid);  
    }
}

/**
 * @brief Retrive next ready client process 
 */
pid_t next_ready_proc() {
    nproc--;
    assert(ready_procs[head]);    
    pid_t ret = ready_procs[head];
    ready_procs[head] = 0;
    head = (head+1)%MAX_PROCESS_NUM;
    return ret;
}
bool has_ready_proc() {
    return nproc > 0;
}

/**
 * @brief unpin pages in iov list
 *
 */
void unpin_iov(sos_addrspace_t *as, iovec_t *iov) {
    iovec_t *cur;
    while(iov) {
        cur = iov;
        iov = iov->next;
        pte_t *pt = as_lookup_pte(as, cur->vstart);
        if (pt) {
            pt->pinned = false;
        }
    }
}

void iov_free(iovec_t *iov) {
    iovec_t *cur;
    while(iov) {
        cur = iov;
        iov = iov->next;
        free(cur);
    }
}

/**
 * @brief same to syscall_end_continuation, but return a 64 bit value
 *
 */
void syscall_end_continuation64(sos_proc_t *proc, uint64_t retval, bool success) {
    assert(proc);
    seL4_MessageInfo_t reply;
    dprintf(4, "ENDING SYSCALL\n", retval);
    dprintf(4, "[SYSEND] Returning %d\n", retval);
    size_t message_length = umax(proc->cont.reply_length, 2);
    if (success) {
        reply = seL4_MessageInfo_new(seL4_NoFault, 0, 0, message_length);
    } else {
        reply = seL4_MessageInfo_new(seL4_UserException, 0, 0, message_length);
    }
    seL4_SetMR(0, retval & 0xffffffff);
    seL4_SetMR(1, retval>>32);
    seL4_SetTag(reply);
    assert(proc->cont.reply_cap != seL4_CapNull);
    seL4_Send(proc->cont.reply_cap, reply);
    iov_free(proc->cont.iov);
    memset(&proc->cont, 0, sizeof(cont_t));
    dprintf(4, "SYSCALL ENDED\n", retval);
}

/**
 * @brief Reply to client
 *
 * @param proc client process to reply
 * @param retval reply value
 * @param success True: reply NoFault; False reply UserException
 */
void syscall_end_continuation(sos_proc_t *proc, int retval, bool success) {
    assert(proc);
    seL4_MessageInfo_t reply;
    dprintf(4, "ENDING SYSCALL\n", retval);
    dprintf(4, "[SYSEND] Returning %d\n", retval);
    size_t message_length = umax(proc->cont.reply_length, 1);
    if (success) {
        reply = seL4_MessageInfo_new(seL4_NoFault, 0, 0, message_length);
    } else {
        reply = seL4_MessageInfo_new(seL4_UserException, 0, 0, message_length);
    }
    seL4_SetMR(0, retval);
    seL4_SetTag(reply);
    assert(proc->cont.reply_cap != seL4_CapNull);
    seL4_Send(proc->cont.reply_cap, reply);
    iov_free(proc->cont.iov);
    memset(&proc->cont, 0, sizeof(cont_t));
    dprintf(4, "SYSCALL ENDED\n", retval);
}

/**
 * @brief Check whether a client address is in valid region
 *
 */
static bool check_region(sos_addrspace_t *as, client_vaddr page, iop_direction_t dir) {
    sos_region_t* reg = as_vaddr_region(as, page);
    if (!reg) {
        return false;
    }
    // Ensure client process has correct permissions to the page
    if (!(reg->rights & seL4_CanWrite) && dir == WRITE) {
        return false;
    } else if (!(reg->rights & seL4_CanRead) && dir == READ) {
        return false;
    }
    return true;
}

/**
 * Pack message bytes into seL4_words
 * @param msgdata message to be printed
 * @param count length of the message
 * @returns the number of bytes encoded
 */
void ipc_write_bin(int start, char* msgdata, size_t length) {
    size_t mr_idx,i,j;
    seL4_Word pack;
    i = 0;
    seL4_SetMR(start, length);
    mr_idx = start + 1;
    while(i < length) {
        pack = 0;
        j = sizeof(seL4_Word);
        while (j > 0 && i < length) {
            pack = pack | ((seL4_Word)msgdata[i] << ((--j)*8));
            i++;
        }
        seL4_SetMR(mr_idx, pack);
        mr_idx++;
    }
}

/**
 * Unpack characters from seL4_Words and stop when meeting null byte.
 * @param msgBuf starting point of buffer to store contents.
 * @param packed_data word packed with 4 bytes
*/
static int unpack_word_str(char* msgBuf, seL4_Word packed_data) {
    int length = 0;
    int j = sizeof(seL4_Word);
    while (j > 0) {
        // Unpack data encoded 4-chars per word.
        *msgBuf = (char)(packed_data >> ((--j) * 8));
        if (*msgBuf == 0) {
            return length;
        }
        length++;
        msgBuf++;
    }
    return length;
}

/**
 * @brief Read a string from IPC buffer
 *
 */
void ipc_read_str(int start, char *buf) {
    assert(buf && start > 0);
    int k = 0, i ;
    for (i = start; i < seL4_MsgMaxLength; i++) {
       int len = unpack_word_str(buf+k, seL4_GetMR(i)); 
       k += len;
       if (len < sizeof(seL4_Word)) break;
    }
    if (k < MAX_FILE_PATH_LENGTH) {
        buf[k] = 0;
    }
}

/**
 * @brief Check whether a callback has been expired
 *
 */
bool callback_valid(callback_info_t *cb) {
    assert(cb);
    sos_proc_t *proc = process_lookup(cb->pid);
    dprintf(3, "got cb with pid, %d and time %llu, process start: %llu \n", cb->pid, cb->start_time, proc->start_time);
    if (proc == NULL) {
        return false;
    }
    if (cb->start_time < proc->start_time) {
        return false;
    }
    return true;
}

/**
 * @brief Ensure given io vector is in memory, similar as vm_fault except it doesn't consider elf loading
 *
 * @param iov io vector
 */
void iov_ensure_loaded(iovec_t iov) {
    sos_addrspace_t *as = effective_process()->vspace;
    sos_region_t *reg = as_vaddr_region(as, iov.vstart);
    assert(reg); // addr in iov must already have been checked
    if (as_page_exists(as, iov.vstart)) {
        if (swap_is_page_swapped(as, iov.vstart)) {
            swap_in_page(iov.vstart);
            as_reference_page(current_process()->vspace, iov.vstart, reg->rights);
            current_process()->cont.page_eviction_process = NULL;
        } else if (!is_referenced(as, iov.vstart)) {
            as_reference_page(as, iov.vstart, reg->rights);
        }
    } else {
        process_create_page(iov.vstart, reg->rights);
    }
}

iovec_t* iov_create(seL4_Word vstart, size_t sz, iovec_t *iohead, iovec_t *iotail, bool sos_iov_flag) {
    iovec_t *ionew = malloc(sizeof(iovec_t));
    if (ionew == NULL) {
        iov_free(iohead);
        return NULL;
    }
    ionew->vstart = vstart;
    ionew->sz = sz;
    ionew->next = NULL;
    ionew->sos_iov_flag = sos_iov_flag;

    if (iohead == NULL) {
        return ionew;
    } else {
        iotail->next = ionew;
        return iohead;
    }
}

/**
 * @brief Transfer a char buffer to an io vector list
 *
 * @param buf char buffer
 * @param nbyte buffer size
 * @param dir IO type, read or write 
 *
 * @return io vector list
 */
iovec_t *cbuf_to_iov(client_vaddr buf, size_t nbyte, iop_direction_t dir) {
    bool vstart_okay;
    size_t remaining = nbyte;
    iovec_t *iohead = NULL;
    iovec_t *iotail = NULL;
    sos_addrspace_t* as = current_as();
    /*Check validness of client addr*/
    if (remaining == 0) {
        vstart_okay = check_region(as, buf, dir);
        if (vstart_okay == false) {
            ERR("Client page lookup %x failed\n", buf);
            return NULL;
        }
        iohead = iov_create(buf, 0, NULL, NULL, false);
        return iohead;
    }
    dprintf(1, "cbuf_to_iov: %d bytes\n", nbyte);
    /*Retrive page information in client addr range into io vector list,
     * and check validness of every page*/
    while(remaining) {
        size_t offset = ((unsigned)buf % PAGE_SIZE);
        size_t buf_delta = umin((PAGE_SIZE - offset), remaining);
        vstart_okay = check_region(as, buf, dir);
        if (vstart_okay == false) {
            ERR("Client page lookup %x failed\n", buf);
            return NULL;
        }
        dprintf(1, "cbuf_to_iov: delta=%d\n", buf_delta);
        iohead = iov_create(buf, buf_delta, iohead, iotail, false);
        if (iotail == NULL) iotail = iohead;
        else iotail = iotail->next;

        if (iohead == NULL) {
            ERR("Insufficient memory to create new iovec\n");
            return NULL;
        }
        assert(iotail);
        remaining -= buf_delta;
        buf += buf_delta;
    }
    int cnt = 0;
    for (iovec_t* iov = iohead; iov; iov = iov->next) {
       cnt += iov->sz; 
    }
    dprintf(1, "cbuf_to_iov: iov=%d, nbyte=%d\n", cnt, nbyte);
    assert(cnt == nbyte);
    return iohead;
}

io_device_t* device_handler_str(const char* filename) {
    if (strcmp(filename, "console") == 0) {
        return &serial_io;
    } else {
        return &nfs_io;
    }
}

static io_device_t* device_handler_fd(int fd) {
    sos_proc_t *curproc = current_process();
    assert(curproc);
    assert(curproc->fd_table);
    return curproc->fd_table[fd]->io;
}

/********************** syscall implementation ***************************/

// TODO check syscall arguments

int sos__sys_open(void) {
    const char *path = current_process()->cont.path;
    fmode_t mode = current_process()->cont.file_mode;
    io_device_t *dev = device_handler_str(path);
    assert(dev);
    return dev->open(path, mode);
}

int sos__sys_getpid(void) {
    syscall_end_continuation(current_process(), current_process()->pid, true);
    return 0;
}

int sos__sys_read(void){
    int file = current_process()->cont.fd;
    size_t nbyte = current_process()->cont.length_arg;
    if (nbyte == 0) {
        syscall_end_continuation(current_process(), 0, true);
        return 0;
    }
    of_entry_t *of = fd_lookup(current_process(), file);
    if (of == NULL) {
        return EINVAL;
    }
    if (!(of->mode & FM_READ)) {
        return EPERM;
    }
    io_device_t *dev = device_handler_fd(file);
    assert(dev);
    return dev->read(current_process()->cont.iov, file, nbyte);
}

int sos__sys_write(void) {
    int file = current_process()->cont.fd;
    size_t nbyte = current_process()->cont.length_arg;
    dprintf(4, "sos__sys_write %d, %d\n", nbyte, file);
    of_entry_t *of = fd_lookup(current_process(), file);
    if (of == NULL) {
        ERR("sos__sys_write of is NULL\n");
        return 1;
    }
    if (!(of->mode & FM_WRITE)) {
        ERR("sos__sys_write mode is not FM_WRITE\n");
        return EPERM;
    }
    if (nbyte == 0) {
        WARN("sos__sys_write 0 nbyte \n");
        syscall_end_continuation(current_process(), 0, true);
        return 0;
    }
    io_device_t *dev = device_handler_fd(file);
    assert(dev);
    return dev->write(current_process()->cont.iov, file,nbyte);
}

int sos__sys_stat(void) {
    return nfs_io.stat();
}

int sos__sys_getdirent(void) {
    return nfs_io.getdirent();
}

int sos__sys_waitpid(void) {
    int err = 0;
    pid_t pid = current_process()->cont.pid;
    sos_proc_t* cur_proc = current_process();
    if (cur_proc->waiting_pid) {
        ERR("%d is already waiting on cur_proc->waiting_pid: %d\n", cur_proc->pid, cur_proc->waiting_pid);
        assert(cur_proc->waiting_pid == 0);
    }
    if (pid == -1) { // waiting all other running proc
        cur_proc->waiting_pid = pid;
        err = register_to_all_proc(cur_proc->pid);
    } else {
        sos_proc_t *ch_proc = process_lookup(pid);
        if (!ch_proc) { // target process is already dead
            syscall_end_continuation(cur_proc, 0, true);
        } else { // register itself to target process's waiter queue
            cur_proc->waiting_pid = pid;
            err = register_to_proc(ch_proc, cur_proc->pid);
        }
    }
    dprintf(3, "sos__sys_waitpid end %d\n", err);
    return err;
}

int sos__sys_proc_delete(void) {
    pid_t pid = current_process()->cont.pid;
    sos_proc_t* proc = process_lookup(pid);
    if (proc == NULL) return EINVAL;
    pid_t saved_pid = current_process()->pid;
    set_current_process(pid);
    process_delete(proc);
    if (saved_pid != pid) { // Only reply client when it is not killing itself
        set_current_process(saved_pid);
        syscall_end_continuation(process_lookup(saved_pid), 0, true);
    }
    return 0;
}

/**
 * Close an open file.
 */
int sos__sys_close(void) {
    int file = current_process()->cont.fd;
    if (fd_lookup(current_process(), file) == NULL) {
        return EINVAL;
    }
    int err;
    of_entry_t* of = fd_lookup(current_process(), file);
    if (of == NULL) {
        return EINVAL;
    }
    io_device_t *io = of->io;
    if (io == NULL) {
        err = fd_free(current_process()->fd_table, file);
    } else if (io->close == NULL) { // NFS file
        err = fd_free(current_process()->fd_table, file);
    } else {
        err = io->close(file); // serial device
    }
    if (!err) {
        syscall_end_continuation(current_process(), 0, true);
        return 0;
    } else {
        return err;
    }
}

int sos__sys_proc_create(void) {
    pid_t pid = start_process(current_process()->cont.path, _sos_ipc_ep_cap);
    if (pid > 0) {
        syscall_end_continuation(current_process(), pid, true);
        return 0;
    } else {
        return EFAULT;
    }
}

int sos__sys_proc_status(void) {
    sos_proc_t *proc = current_process();

    while (proc->cont.iov) {
        iov_ensure_loaded(*proc->cont.iov);
        sos_vaddr dst = as_lookup_sos_vaddr(proc->vspace, proc->cont.iov->vstart);
        if (dst == 0) {
            free(proc->cont.proc_stat_buf);
            return EINVAL;
        }
        memcpy((char*)dst, proc->cont.proc_stat_buf, proc->cont.iov->sz);
        proc->cont.proc_stat_buf += proc->cont.iov->sz;
        proc->cont.iov = proc->cont.iov->next;
    }

    free(proc->cont.proc_stat_buf);
    syscall_end_continuation(current_process(), proc->cont.proc_stat_n, true);
    return 0;
}

int sos__sys_brk(void) {
    sos_addrspace_t* as = proc_as(current_process());
    assert(as);
    client_vaddr brk = sos_brk(as, current_process()->cont.brk);
    if (brk == 0) {
        return EFAULT;
    } else {
        syscall_end_continuation(current_process(), brk, true);
        return 0;
    }
}

static void sys_notify_client(uint32_t id, void *data) {
    pid_t pid = (pid_t)data;
    sos_proc_t * proc = process_lookup(pid);
    if (proc) // If the sleeping client is still alive 
        syscall_end_continuation(proc, 0, true);
}

int sos__sys_usleep(void) {
    if (current_process()->cont.delay == 0) {
        syscall_end_continuation(current_process(), 0, true);
    }
    int err = register_timer(current_process()->cont.delay, sys_notify_client, (int*)current_process()->pid);
    return err;
}

int sos__sys_timestamp(void) {
    uint64_t tick = time_stamp();
    syscall_end_continuation64(current_process(), tick, true);
    return 0;
}
