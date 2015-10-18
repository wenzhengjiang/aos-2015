/**
 * @file swap.c
 * @brief management of swap file space
 */

#include <stdbool.h>
#include <setjmp.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <nfs/nfs.h>
#include <limits.h>
#include <clock/clock.h>

#include "swap.h"
#include "process.h"
#include "network.h"
#include "syscall.h"

#define verbose 0
#include <log/debug.h>
#include <log/panic.h>

#define SWAP_FILE ".sos_swap"
#define ALIGNED(page) (page % PAGE_SIZE == 0)
#define VADDR_TO_SADDR(vaddr) ((vaddr-swap_table)*PAGE_SIZE)

static const int swap_generic_error = -2;
#define OPEN -1

static fhandle_t swap_handle;
static bool inited = false;

/*free page space list*/
static swap_entry_t * free_list;
/*swap table: each entry in swap table represents a page size space in swap file*/
static swap_entry_t * swap_table;
extern jmp_buf ipc_event_env;

/**
 * @brief   Allocate a swap page space. 
 *          Takes O(1) time to get a free space from free_list 
 *          Kill current client if it fails to allocate it
 *
 * @return offset in swap file
 */
static swap_addr swap_alloc(void) {
    if (free_list == NULL) {
        ERR("swap file is full !");
        process_delete(current_process());
        longjmp(ipc_event_env, -1);
    } else {
        swap_addr ret = VADDR_TO_SADDR(free_list);
        free_list = free_list->next_free;
        return ret;
    }
}

/**
 * @brief Free a swap page space. O(1)
 *
 * @param saddr swap page offset
 */
void swap_free(swap_addr saddr) {
   assert(ALIGNED(saddr));
   swap_table[saddr/PAGE_SIZE].next_free = free_list; 
   free_list = &swap_table[saddr/PAGE_SIZE];
}

/**
 * @brief swap file creation callback. Stop sos if it fails to create swap file
 */
static void
sos_nfs_swap_create_callback(uintptr_t cb, enum nfs_stat status, fhandle_t *fh,
                        fattr_t *fattr) {
    dprintf(4, "[SWAP] Invoking nfs_create callback\n");
    pid_t token = ((callback_info_t*)cb)->pid;
    set_current_process(token);
    if (!callback_valid((callback_info_t*)cb)) {
        free((callback_info_t*)cb);
        return;
    }
    free((callback_info_t*)cb);

    sos_proc_t *proc = current_process();
    if (status != NFS_OK) {
        ERR("[SWAP] Failed to create swap file\n");
        return;
    }
    swap_handle = *fh;
    inited = true;
    add_ready_proc(proc->pid);
    return;
}

/**
 * @brief open swap file callback
 */
static void sos_swap_open(void) {
    uint32_t clock_upper = time_stamp() >> 32;
    uint32_t clock_lower = (time_stamp() << 32) >> 32;
    struct sattr default_attr = {.mode = 0x7,
                                     .uid = 0,
                                     .gid = 0,
                                     .size = 0,
                                     .atime = {clock_upper, clock_lower},
                                     .mtime = {clock_upper, clock_lower}};

    sos_proc_t *proc = current_process();
    proc->cont.swap_status = SWAP_RUNNING;
    pid_t pid = proc->pid;
    dprintf(2, "[SWAP] Calling nfs_create\n");

    callback_info_t *cb = malloc(sizeof(callback_info_t));
    if (!cb) {
        ERR("[SWAP] nfs_create failed\n");
        proc->cont.swap_status = SWAP_FAILED;
        longjmp(ipc_event_env, swap_generic_error);
    }
    cb->pid = pid;
    cb->start_time = time_stamp();
    if(nfs_create(&mnt_point, SWAP_FILE, &default_attr, sos_nfs_swap_create_callback,
                  (uintptr_t)cb)) {
        free((callback_info_t*)cb);
        ERR("[SWAP] nfs_create failed\n");
        proc->cont.swap_status = SWAP_FAILED;
        longjmp(ipc_event_env, swap_generic_error);
    }
}

/**
 * @brief similar to nfs_write_callback, except it resume the process when writing complete 
 */
static void
swap_write_callback(uintptr_t cb, enum nfs_stat status, fattr_t *fattr, int count) {
    dprintf(4, "[SWAP] Write callback\n");
    pid_t token = ((callback_info_t*)cb)->pid;
    set_current_process(token);
    if (!callback_valid((callback_info_t*)cb)) {
        return;
    }

    sos_proc_t *proc = current_process();

    if (status != NFS_OK) {
        ERR("[SWAP] Failed to write to swap file");
        proc->cont.swap_status = SWAP_FAILED;
        free((callback_info_t*)cb);
        return;
    }
    proc->cont.swap_cnt += count;

    dprintf(3, "[SWAP] write callback: sos addr: %x, offset: %u, proc->cont.swap_cnt: %u, count: %d\n",
            proc->cont.swap_page, proc->cont.swap_file_offset, proc->cont.swap_cnt, count);
    assert(proc->cont.swap_cnt <= PAGE_SIZE);
    if (proc->cont.swap_cnt == PAGE_SIZE) {
        proc->cont.swap_status = SWAP_SUCCESS;
        proc->cont.swap_cnt = 0;
        free((callback_info_t*)cb);
        add_ready_proc(proc->pid);
        return;
    }
    int cnt = proc->cont.swap_cnt;
    dprintf(3, "[SWAP] Asking to write PAGE_SIZE - cnt (%u) bytes\n", PAGE_SIZE - cnt);
    if (nfs_write(&swap_handle, proc->cont.swap_file_offset + cnt, PAGE_SIZE - cnt,
                  (void*)(proc->cont.swap_page+cnt), swap_write_callback,
                  (uintptr_t)cb)) {
        proc->cont.swap_status = SWAP_FAILED;
        return;
    }
}

/**
 * @brief swap write
 *
 * @param page page need to be swapped out
 *
 * @return swap file offset
 */
swap_addr sos_swap_write(sos_vaddr page) {
    assert(ALIGNED(page));
    sos_proc_t *proc = current_process();

    proc->cont.swap_status = SWAP_RUNNING;
    dprintf(3, "[SWAP] Swap write invoked\n");
    if (!inited) {
        sos_swap_open();
        longjmp(ipc_event_env, -1);
    }
    pid_t pid = proc->pid;
    proc->cont.swap_page = page;
    proc->cont.swap_file_offset = swap_alloc();
    assert(ALIGNED(proc->cont.swap_file_offset));
    // compute chksum
    {
        int code = 0;
        for (int i = 0; i < PAGE_SIZE; i++) {
            code += ((char*)page)[i];
        }
        swap_table[proc->cont.swap_file_offset/PAGE_SIZE].chksum = code;
    }
    dprintf(3, "[SWAP] Asking to write PAGE_SIZE - cnt (%u) bytes\n", PAGE_SIZE);

    callback_info_t *cb = malloc(sizeof(callback_info_t));
    if (!cb) {
        ERR("Unable to create callback\n");
        longjmp(ipc_event_env, -1);
    }
    cb->pid = pid;
    cb->start_time = time_stamp();

    if (nfs_write(&swap_handle, proc->cont.swap_file_offset, PAGE_SIZE,
                  (const void*)proc->cont.swap_page, swap_write_callback,
                  (uintptr_t)cb) != RPC_OK) {
        proc->cont.swap_status = SWAP_FAILED;
        free((callback_info_t*)cb);
        process_delete(current_process());
        longjmp(ipc_event_env, swap_generic_error);
    }
    proc->cont.swap_write_fired = true;
    return proc->cont.swap_file_offset;
}

static void
swap_read_callback(uintptr_t cb, enum nfs_stat status,
                      fattr_t *fattr, int count, void* data) {
    dprintf(4, "[SWAP] Read callback\n");
    assert(cb);
    pid_t token = ((callback_info_t*)cb)->pid;
    set_current_process(token);
    if (!callback_valid((callback_info_t*)cb)) {
        free((callback_info_t*)cb);
        return;
    }
    free((callback_info_t*)cb);

    (void)fattr;
    sos_proc_t *proc = current_process();
    assert(proc);
    if (status != NFS_OK) {
        ERR("Failed to read from swap file\n");
        process_delete(proc);
        return;
    }
    if (count == 0) {
        ERR("Failed to read from swap file\n");
        process_delete(proc);
        return;
    }

    assert(count == PAGE_SIZE);
    assert(proc->cont.swap_status != SWAP_SUCCESS);
    proc->cont.swap_status = SWAP_SUCCESS;
    add_ready_proc(proc->pid);
    memcpy((char*)proc->cont.swap_page, (char*)data, count);
    { // check chksum
        int code = 0;
        for (int i = 0; i < PAGE_SIZE; i++) {
            code += ((char*)proc->cont.swap_page)[i];
        }
        if(swap_table[proc->cont.swap_file_offset/PAGE_SIZE].chksum != code) {
            ERR("The page swapped in was broken !");
            return ;
        }
    }
    swap_free(proc->cont.swap_file_offset);
    dprintf(3, "[SWAP] Leaving read callback\n");
}

/**
 * @brief swap in
 *
 * @param page vaddr of page needs to be swapped in
 * @param pos position of page in swap file
 */
void sos_swap_read(sos_vaddr page, swap_addr pos) {
    assert(inited);
    assert(ALIGNED(page));
    assert(ALIGNED(pos));
    dprintf(3, "[SWAP] Reading: %x from %u\n", page, pos);
    sos_proc_t *proc = current_process();
    proc->cont.swap_status = SWAP_RUNNING;
    proc->cont.swap_page = page;
    proc->cont.swap_file_offset = pos;
    dprintf(4, "[SWAP] swap_read addr=%x,pos=%x\n", page, pos);
    dprintf(4, "[SWAP] pid=%d\n", proc->pid);
    pid_t pid = proc->pid;

    callback_info_t *cb = malloc(sizeof(callback_info_t));
    if (!cb) {
        ERR("[SWAP] Read failed\n");
        proc->cont.swap_status = SWAP_FAILED;
        longjmp(ipc_event_env, swap_generic_error);
    }
    cb->pid = pid;
    cb->start_time = time_stamp();

    if(nfs_read(&swap_handle, pos, PAGE_SIZE, swap_read_callback, (uintptr_t)cb)) {
        ERR("[SWAP] Read failed\n");
        proc->cont.swap_status = SWAP_FAILED;
        free((callback_info_t*)cb);
        longjmp(ipc_event_env, swap_generic_error);
    }
}

void swap_init(void * vaddr) {
    swap_table = (swap_entry_t*) vaddr;
    free_list = swap_table;
    for (int i = 0; i < NSWAP; i++) {
        swap_table[i].chksum = 0;
        swap_table[i].next_free = &swap_table[i+1];
    }
    swap_table[NSWAP].next_free = NULL;
}
