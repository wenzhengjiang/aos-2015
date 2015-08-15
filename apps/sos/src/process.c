/** process.c --- Process management **/

#include <sel4/sel4.h>
#include <stdlib.h>
#include <device/vmem_layout.h>
#include "process.h"
#include <errno.h>
#include <assert.h>

sos_proc_t test_proc;
sos_proc_t *curproc = &test_proc;

/**
 * Create a region for the stack
 * @return pointer to the new stack region
 */
static sos_region_t* init_stack(void) {
    sos_region_t* region;
    region = (sos_region_t*)malloc(sizeof(sos_region_t));
    if (region == NULL) {
        return NULL;
    }
    region->start = PROCESS_STACK_TOP - (1 << seL4_PageBits);
    region->end = PROCESS_STACK_TOP;
    region->perms = seL4_AllRights;
    return region;
}

/**
 * Create a region for the IPC buffer
 * @return pointer to the new IPC Buffer region
 */
static sos_region_t* init_ipc_buf(void) {
    sos_region_t* region;
    region = (sos_region_t*)malloc(sizeof(sos_region_t));
    if (region == NULL) {
        return NULL;
    }
    region->start = PROCESS_IPC_BUFFER;
    // TODO: What's the size of the IPC buffer?
    region->end = PROCESS_VMEM_START - 1;
    region->perms = seL4_AllRights;
    region->next = NULL;
    return region;
}

/**
 * Create statically positioned regions
 * @return error code or 0 for success
 */
static int init_regions(void) {
    sos_region_t* ipc_buf = init_ipc_buf();
    sos_region_t* stack = init_stack();
    if (stack || ipc_buf == NULL) {
        return ENOMEM;
    }
    ipc_buf->next = stack;
    curproc->vspace.regions = ipc_buf;
    return 0;
}

/**
 * Create a new process
 * @return error code or 0 for success
 */
int init_process(void) {
    int err;
    err = init_regions();
    if (err) {
        return err;
    }
    return 0;
}
