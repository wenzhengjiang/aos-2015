#ifndef _SOS_SWAP_H_
#define _SOS_SWAP_H_

#include <nfs/nfs.h>
#include "sos_type.h"

typedef struct swap_entry {
    struct swap_entry * next_free;
    int chksum;
} swap_entry_t;

#define SWAP_FILE_SIZE 2147483648  // maximum size of swap file is 2G
#define NSWAP (SWAP_FILE_SIZE / PAGE_SIZE) // number of entries in swap table
#define SWAP_TABLE_SIZE (NSWAP * sizeof(swap_entry_t))

#define SWAP_SUCCESS (1)
#define SWAP_RUNNING (0)
#define SWAP_FAILED  (-1)

// offset in swap file
typedef seL4_Word swap_addr;

void swap_init(void *);
swap_addr sos_swap_write(sos_vaddr page);
void sos_swap_read(sos_vaddr page, swap_addr pos);
void swap_free(swap_addr saddr);

#endif
