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

#define verbose 5
#include <log/debug.h>
#include <log/panic.h>

#define SWAP_FILE ".sos_swap"
#define ALIGNED(page) (page % PAGE_SIZE == 0)
#define VADDR_TO_SADDR(vaddr) ((vaddr-swap_table)*PAGE_SIZE)
static const int swap_generic_error = -2;
#define OPEN -1

static fhandle_t swap_handle;
static bool inited = false;

static swap_entry_t * free_list;
static swap_entry_t * swap_table;
extern jmp_buf ipc_event_env;
extern bool callback_done ;

// return offset in swap file
static swap_addr swap_alloc(void) {
    if (free_list == NULL) return swap_generic_error;        
    else {
        swap_addr ret = VADDR_TO_SADDR(free_list);
        free_list = free_list->next_free;
        return ret;
    }
}

static void swap_free(swap_addr saddr) {
   assert(ALIGNED(saddr));
   swap_table[saddr/PAGE_SIZE].next_free = free_list; 
   free_list = &swap_table[saddr/PAGE_SIZE];
}

static void
sos_nfs_swap_create_callback(uintptr_t token, enum nfs_stat status, fhandle_t *fh,
                        fattr_t *fattr) {
    sos_proc_t *proc = current_process();
    printf("nfs_create callback\n");
    if (status != NFS_OK) {
        dprintf(4, "failed to create swap file");
        proc->cont.swap_status = SWAP_FAILED;
        return;
    }
    swap_handle = *fh;
    dprintf(2, "sos_nfs_swap_create_callback");
    inited = true;
    callback_done = true;
    return;
}

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
    printf("Calling nfs_create\n");
    if(nfs_create(&mnt_point, SWAP_FILE, &default_attr,
                sos_nfs_swap_create_callback, pid)) {
        printf("nfs_create failed\n");
        proc->cont.swap_status = SWAP_FAILED;
        longjmp(ipc_event_env, swap_generic_error);
    }
}

static void
swap_write_callback(uintptr_t token, enum nfs_stat status, fattr_t *fattr, int count) {
    sos_proc_t *proc = process_lookup(token);
    if (status != NFS_OK) {
        dprintf(4, "faile to write to swap file");
        proc->cont.swap_status = SWAP_FAILED;
        return;
    }
    printf("WRITE callback addr=%x,pos=%d, cnt=%d\n", proc->cont.swap_page+proc->cont.swap_cnt, 
            proc->cont.swap_file_offset+proc->cont.swap_cnt, count);
    proc->cont.swap_cnt += count;
    int zero_count = 0;
    for (int i = 0; i < PAGE_SIZE; i++) {
        if (((char*)proc->cont.swap_page)[i] == 0) {
            zero_count++;
        }
    }
    printf("write callback: sos addr: %x, offset: %u, proc->cont.swap_cnt: %u, zeroes: %d, count: %d\n",proc->cont.swap_page,
           proc->cont.swap_file_offset, proc->cont.swap_cnt, zero_count, count);
    if (proc->cont.swap_cnt == PAGE_SIZE) {
        proc->cont.swap_status = SWAP_SUCCESS;
        proc->cont.swap_cnt = 0;
        callback_done = true;
        //printf("Finishing with write callback1\n");
        return;
    } else {
        int cnt = proc->cont.swap_cnt;
        if (nfs_write(&swap_handle, proc->cont.swap_file_offset + cnt, PAGE_SIZE - cnt,
                      (void*)(proc->cont.swap_page+cnt), swap_write_callback,
                     token)) {
            proc->cont.swap_status = SWAP_FAILED;
            return;
        }
    }
}

swap_addr sos_swap_write(sos_vaddr page) {
    assert(ALIGNED(page));
    sos_proc_t *proc = current_process();
    proc->cont.swap_status = SWAP_RUNNING;
    printf("In SSW\n");
    if (!inited) {
        printf("opening\n");
        sos_swap_open();
        printf("jumping\n");
        longjmp(ipc_event_env, -1);
    }
    printf("Is open\n");
    pid_t pid = proc->pid;

    proc->cont.swap_page = page;
    proc->cont.swap_file_offset = swap_alloc();
    assert(ALIGNED(proc->cont.swap_file_offset));
    int code = 0;
    for (int i = 0; i < PAGE_SIZE; i++) {
        code += ((char*)page)[i];
    }
    swap_table[proc->cont.swap_file_offset/PAGE_SIZE].chksum = code;
    if (proc->cont.swap_file_offset < 0) return proc->cont.swap_file_offset;
    assert(proc->cont.swap_cnt == 0);
    if (nfs_write(&swap_handle, proc->cont.swap_file_offset, PAGE_SIZE,
                  (const void*)proc->cont.swap_page, swap_write_callback, pid) != RPC_OK) {
        proc->cont.swap_status = SWAP_FAILED;
        longjmp(ipc_event_env, swap_generic_error);
    }  else  {
        return proc->cont.swap_file_offset;
    }
}

static void
swap_read_callback(uintptr_t token, enum nfs_stat status,
                      fattr_t *fattr, int count, void* data) {
    if (count == 0) return;
    (void)fattr;
    sos_proc_t *proc = process_lookup(token);
    if (status != NFS_OK) {
        proc->cont.swap_status = SWAP_FAILED;
        dprintf(5, "failed to read from swap file");
        return;
    }
    assert(count == PAGE_SIZE);
    assert(proc->cont.swap_status != SWAP_SUCCESS);
    proc->cont.swap_status = SWAP_SUCCESS;
    callback_done = true;
    memcpy((char*)proc->cont.swap_page, (char*)data, count);
    int code = 0;
    for (int i = 0; i < PAGE_SIZE; i++) {
        code += ((char*)proc->cont.swap_page)[i];
    }
    assert(swap_table[proc->cont.swap_file_offset/PAGE_SIZE].chksum == code);
    swap_free(proc->cont.swap_file_offset);
}

void sos_swap_read(sos_vaddr page, swap_addr pos) {
    assert(inited);
    assert(ALIGNED(page));
    assert(ALIGNED(pos));
    printf("Reading: %x from %u\n", page, pos);
    sos_proc_t *proc = current_process();
    proc->cont.swap_status = SWAP_RUNNING;
    proc->cont.swap_page = page;
    proc->cont.swap_file_offset = pos;
    printf("swap_read addr=%x,pos=%d\n", page, pos);
    if(nfs_read(&swap_handle, pos, PAGE_SIZE, swap_read_callback, proc->pid)) {
        printf("read failed\n");
        proc->cont.swap_status = SWAP_FAILED;
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
