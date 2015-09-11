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

#define verbose 0
#include <log/debug.h>
#include <log/panic.h>

#define SWAP_FILE ".sos_swap"
#define ALIGNED(page) (page % PAGE_SIZE == 0)
#define VADDR_TO_SADDR(vaddr) ((vaddr-swap_table)*PAGE_SIZE)
#define SUCC 1
#define FAIL 2

static fhandle_t swap_handle;
static jmp_buf read_env, write_env, open_env;
static bool inited = false;

static swap_entry_t * free_list;
static swap_entry_t * swap_table;

// return offset in swap file
static swap_addr swap_alloc(void) {
    if (free_list == NULL) return -1;        
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
    (void)token;
    if (status != NFS_OK) {
        dprintf(5, "failed to create swap file");
        longjmp(open_env, FAIL);
    }

    swap_handle = *fh;
    dprintf(2, "sos_nfs_swap_create_callback");
    longjmp(open_env, SUCC);
}

static void sos_swap_open(void) {
    int ret ;
    uint32_t clock_upper = time_stamp() >> 32;
    uint32_t clock_lower = (time_stamp() << 32) >> 32;
    struct sattr default_attr = {.mode = 0x7,
                                     .uid = 0,
                                     .gid = 0,
                                     .size = 0,
                                     .atime = {clock_upper, clock_lower},
                                     .mtime = {clock_upper, clock_lower}};

    if ((ret = setjmp(open_env)) == 0) {
        nfs_create(&mnt_point, SWAP_FILE, &default_attr, sos_nfs_swap_create_callback, 0);
        while (1);
    } 
    assert(ret == SUCC);
}

// stupid names
static sos_vaddr src_vaddr, dest_vaddr;
static swap_addr swap_offset;
static int write_cnt ;

static void
swap_write_callback(uintptr_t token, enum nfs_stat status, fattr_t *fattr, int count) {
    if (status != NFS_OK) {
        dprintf(5, "faile to write to swap file");
        longjmp(write_env, FAIL);
    }
    write_cnt += count;
    if (write_cnt == PAGE_SIZE) {
        longjmp(write_env, SUCC); 
    } else {
        src_vaddr += count;
        swap_offset += count;
        int err = nfs_write(&swap_handle, swap_offset, PAGE_SIZE - write_cnt,
                        (const void*)src_vaddr, swap_write_callback,
                        0);
        if (err) longjmp(write_env, FAIL);
    }
}

swap_addr sos_swap_write(sos_vaddr page) {
    if (!inited) {
        sos_swap_open();
        inited = true;
    }
    src_vaddr = page;
    swap_offset = swap_alloc();
    if (swap_offset < 0) 
        return -1;
    swap_addr ret = swap_offset;
    write_cnt = 0;
    int err;
    if ((err = setjmp(write_env)) == 0) {
        err = nfs_write(&swap_handle, swap_offset, PAGE_SIZE,
                        (const void*)src_vaddr, swap_write_callback, 0);
        if (err) return -1;
        while (1) ;
    } else 
        return err == SUCC ?  ret : -1;
}

static void
swap_read_callback(uintptr_t token, enum nfs_stat status,
                      fattr_t *fattr, int count, void* data) {
    (void)fattr;
    assert(inited);
    if (status != NFS_OK) {
        dprintf(5, "failed to read from swap file");
        longjmp(read_env, FAIL);
    }
    assert(count == PAGE_SIZE);
    memcpy((char*)dest_vaddr, (char*)data, count);
    longjmp(read_env, SUCC); 
}

int sos_swap_read(sos_vaddr page, swap_addr pos) {
    assert(ALIGNED(page));
    dest_vaddr = page;
    int err ; 
    if ((err = setjmp(read_env)) == 0) {
        nfs_read(&swap_handle, pos, PAGE_SIZE, swap_read_callback, 0);    
        while (1);
    } else if (err == FAIL) {
        return -1;
    } else {
        swap_free(pos);
        return 0;
    }
}

void swap_init(void * vaddr) {
    swap_table = (swap_entry_t*) vaddr;
    free_list = swap_table;
    for (int i = 0; i < NSWAP-1; i++) {
        swap_table[i].next_free = &swap_table[i+1];
    }
    swap_table[NSWAP-1].next_free = NULL;
}
