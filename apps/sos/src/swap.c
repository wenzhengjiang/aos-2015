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
#define ERR -1
#define OPEN -1

static fhandle_t swap_handle;
static bool inited = false;

static swap_entry_t * free_list;
static swap_entry_t * swap_table;
extern jmp_buf ipc_event_env;

// return offset in swap file
static swap_addr swap_alloc(void) {
    if (free_list == NULL) return ERR;        
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
    if (status != NFS_OK) {
        dprintf(5, "failed to create swap file");
        longjmp(ipc_event_env, ERR);
    }

    swap_handle = *fh;
    dprintf(2, "sos_nfs_swap_create_callback");
    inited = true;

    longjmp(ipc_event_env, token);
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
    pid_t pid = proc->pid;

    if(nfs_create(&mnt_point, SWAP_FILE, &default_attr,
                sos_nfs_swap_create_callback, pid)) {
        longjmp(ipc_event_env, ERR);
    }
}

static void
swap_write_callback(uintptr_t token, enum nfs_stat status, fattr_t *fattr, int count) {
    sos_proc_t *proc = process_lookup(token);

    if (status != NFS_OK) {
        dprintf(5, "faile to write to swap file");
        longjmp(ipc_event_env, ERR);
    }
    proc->cont.swap_cnt += count;
    if (proc->cont.swap_cnt == PAGE_SIZE) {
        longjmp(ipc_event_env, token);
    } else {
        int cnt = proc->cont.swap_cnt;
        if(nfs_write(&swap_handle, proc->cont.swap_file_offset+cnt, PAGE_SIZE-cnt,
                        (const void*)proc->cont.swap_page+cnt, swap_write_callback,
                        token)) {
            longjmp(ipc_event_env, ERR);
        }
        longjmp(ipc_event_env, token);
    }
}

swap_addr sos_swap_write(sos_vaddr page) {
    if (!inited) {
        sos_swap_open();
        return OPEN;
    }
    sos_proc_t *proc = current_process();
    pid_t pid = proc->pid;

    proc->cont.swap_page = page;
    proc->cont.swap_file_offset = swap_alloc();
    assert(proc->cont.swap_cnt == 0);
    if (proc->cont.swap_file_offset < 0) {
        longjmp(ipc_event_env, ERR);
    }

    if (nfs_write(&swap_handle, proc->cont.swap_file_offset, PAGE_SIZE, (const void*)proc->cont.swap_page, swap_write_callback, pid)) {
        longjmp(ipc_event_env, ERR);
    }  else  {
        return proc->cont.swap_file_offset;
    }
}

static void
swap_read_callback(uintptr_t token, enum nfs_stat status,
                      fattr_t *fattr, int count, void* data) {
    (void)fattr;
    if (status != NFS_OK) {
        dprintf(5, "failed to read from swap file");
        longjmp(ipc_event_env, ERR);
    }
    assert(count == PAGE_SIZE);
    sos_proc_t *proc = process_lookup(token);
    memcpy((char*)proc->cont.swap_page, (char*)data, count);
    swap_free(proc->cont.swap_file_offset);

    longjmp(ipc_event_env, token); 
}

void sos_swap_read(sos_vaddr page, swap_addr pos) {
    assert(inited);
    assert(ALIGNED(page));
    sos_proc_t *proc = current_process();
    proc->cont.swap_page = page;
    proc->cont.swap_file_offset = pos;

    if(nfs_read(&swap_handle, pos, PAGE_SIZE, swap_read_callback, proc->pid)) 
        longjmp(ipc_event_env, ERR);
    return ;
}

void swap_init(void * vaddr) {
    swap_table = (swap_entry_t*) vaddr;
    free_list = swap_table;
    for (int i = 0; i < NSWAPERR; i++) {
        swap_table[i].next_free = &swap_table[i+1];
    }
    swap_table[NSWAPERR].next_free = NULL;
}
