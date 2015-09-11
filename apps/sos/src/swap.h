#ifndef _SOS_SWAP_H_
#define _SOS_SWAP_H_

#include <nfs/nfs.h>
#include "sos_type.h"

typedef struct swap_entry {
    struct swap_entry * next_free;
} swap_entry_t;


#define SWAP_FILE_SIZE 5049942016 // maximum size of swap file is 4G
#define NSWAP (SWAP_FILE_SIZE / PAGE_SIZE) // nunber of entries in swap table
#define SWAP_TABLE_SIZE (NSWAP * sizeof(swap_entry_t)) 

typedef int swap_addr;

void swap_init(void *);
swap_addr sos_swap_write(sos_vaddr page);
int sos_swap_read(sos_vaddr page, swap_addr pos);

#endif
