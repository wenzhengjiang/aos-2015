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

#define verbose 5
#include <log/debug.h>
#include <log/panic.h>

#define PRINT_MESSAGE_START 2
#define OPEN_MESSAGE_START 2
#define SERIAL_FD 5

static size_t sos_debug_print(char *data) {
    int count = strlen(data);
    for (int i = 0; i < count; i++) {
        seL4_DebugPutChar(data[i]);
    }
    return count;
}
/**
 * Pack characters of the message into seL4_words
 * @param msgdata message to be printed
 * @param count length of the message
 * @returns the number of characters encoded
 */
static void ipc_write(int start, const char* msgdata, size_t count) {
    size_t mr_idx,i,j;
    seL4_Word pack;
    i = 0;
    mr_idx = start;
    while(i < count) {
        pack = 0;
        j = sizeof(seL4_Word);
        while (j > 0 && i < count) {
            pack = pack | ((seL4_Word)msgdata[i] << ((--j)*8));
            i++;
        }
        seL4_SetMR(mr_idx, pack);
        mr_idx++;
    }
}


int sos_sys_open(const char *path, fmode_t mode) {
    if (!path) {
        sos_debug_print("path is null pointer\n");
        return -1;
    } 
    int len = ((strlen(path)+1) + sizeof(seL4_Word)-1) >> 2;
    if (strlen(path) > MAX_FILE_PATH_LENGTH) {
        sos_debug_print("file name longer than 256\n");
        return -1;
    }
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(seL4_NoFault, 0, 0, 2 + len);
    seL4_SetTag(tag);
    seL4_SetMR(0, (seL4_Word)SOS_SYSCALL_OPEN); 
    seL4_SetMR(1, (seL4_Word)mode);
    ipc_write(OPEN_MESSAGE_START, path, strlen(path)+1);
    seL4_MessageInfo_t reply = seL4_Call(SYSCALL_ENDPOINT_SLOT, tag);
    if (seL4_MessageInfo_get_label(reply) != seL4_NoFault)
        return -1;
    else 
        return seL4_GetMR(0);
}

int sos_sys_read(int file, char *buf, size_t nbyte) {
    if (file < 0 || buf == NULL || nbyte == 0) return 0;

    seL4_MessageInfo_t tag = seL4_MessageInfo_new(seL4_NoFault, 0, 0, 4);
    seL4_SetTag(tag);
    seL4_SetMR(0, (seL4_Word)SOS_SYSCALL_READ); 
    seL4_SetMR(1, (seL4_Word)file);
    seL4_SetMR(2, (seL4_Word)buf);
    seL4_SetMR(3, (seL4_Word)nbyte);
    seL4_MessageInfo_t reply = seL4_Call(SYSCALL_ENDPOINT_SLOT, tag);
    if (seL4_MessageInfo_get_label(reply) != seL4_NoFault)
        return -1;
    else 
        return seL4_GetMR(0);

}

int sos_sys_write(int file, char *buf, size_t nbyte) {
    if (file < 0 || buf == NULL || nbyte == 0) return 0;

    seL4_MessageInfo_t tag = seL4_MessageInfo_new(seL4_NoFault, 0, 0, 4);
    seL4_SetTag(tag);
    seL4_SetMR(0, (seL4_Word)SOS_SYSCALL_WRITE); 
    seL4_SetMR(1, (seL4_Word)file);
    seL4_SetMR(2, (seL4_Word)buf);
    seL4_SetMR(3, (seL4_Word)nbyte);
    seL4_MessageInfo_t reply = seL4_Call(SYSCALL_ENDPOINT_SLOT, tag);
    if (seL4_MessageInfo_get_label(reply) != seL4_NoFault)
        return -1;
    else 
        return seL4_GetMR(0);
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
    int64_t ret = seL4_GetMR(1);
    ret <<= 32;
    ret += seL4_GetMR(0);

    return ret;
}

size_t sos_write(void *data, size_t count) {
    return sos_sys_write(SERIAL_FD, (char*)data, count);
}

size_t sos_read(void *vData, size_t count) {
    return sos_sys_read(SERIAL_FD, vData, count);
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
