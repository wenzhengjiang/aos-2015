/*
 * Copyright 2014, NICTA
 *
 * This software may be distributed and modified according to the terms of
 * the BSD 2-Clause license. Note that NO WARRANTY is provided.
 * See "LICENSE_BSD2.txt" for details.
 *
 * @TAG(NICTA_BSD)
 */

#include <stdarg.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <sos.h>
#include <syscall.h>

#include <sel4/sel4.h>

#define PRINT_MESSAGE_START 2

int sos_sys_open(const char *path, fmode_t mode) {
    assert(!"You need to implement this");
    return -1;
}

int sos_sys_read(int file, char *buf, size_t nbyte) {
    assert(!"You need to implement this");
    return -1;
}

int sos_sys_write(int file, const char *buf, size_t nbyte) {
    assert(!"You need to implement this");
    return -1;
}

/*
 * send : callno, msec
 * rev  : 
 * err  : label != 0 
 */
void sos_sys_usleep(int msec) {
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(seL4_NoFault, 0, 0, 2);
    seL4_SetTag(tag);
    seL4_SetMR(0, SOS_SYSCALL_USLEEP); 
    seL4_SetMR(1, msec); 
    seL4_MessageInfo_t reply = seL4_Call(SYSCALL_ENDPOINT_SLOT, tag);
    assert(seL4_MessageInfo_get_label(reply) == seL4_NoFault);
}

/* send: callno
 * rev : low, high
 * err : reply.label != 0
 */
int64_t sos_sys_time_stamp(void) {
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(seL4_NoFault, 0, 0, 1);
    seL4_SetTag(tag);
    seL4_SetMR(0, SOS_SYSCALL_TIMESTAMP); 
    seL4_MessageInfo_t reply = seL4_Call(SYSCALL_ENDPOINT_SLOT, tag);
    if(seL4_MessageInfo_get_label(reply) != seL4_NoFault) return -1;
    int64_t ret = seL4_GetMR(0);
    ret <<= 32;
    ret += seL4_GetMR(1);

    return ret;
}


pid_t sos_process_create(const char *path) {
    assert(!"sos_process_create not implemented!");
    return 0;
}

int sos_process_status(sos_process_t *processes, unsigned max) {
    assert(!"sos_process_status not implemented!");
    return 0;
}

pid_t sos_process_wait(pid_t pid) {
    assert(!"sos_process_wait not implemented!");
    return 0;
}

int sos_stat(const char *path, sos_stat_t *buf) {
    assert(!"sos_stat implemented!");
    return 0;
}

int sos_getdirent(int pos, char *name, size_t nbyte) {
    assert(!"sos_getdirent implemented!");
    return 0;
}
