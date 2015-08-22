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

static size_t sos_debug_print(char *data) {
    int count = strlen(data);
    for (int i = 0; i < count; i++) {
        seL4_DebugPutChar(data[i]);
    }
    return count;
}
int sos_sys_open(const char *path, fmode_t mode) {
    (void)mode;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(seL4_NoFault, 0, 0, 1);
    seL4_SetTag(tag);
    seL4_SetMR(0, SOS_SYSCALL_OPEN); 
    seL4_Call(SYSCALL_ENDPOINT_SLOT, tag);

    return 5;
}

int sos_sys_read(int file, char *buf, size_t nbyte) {
    return sos_read(buf, nbyte);
}

int sos_sys_write(int file, const char *buf, size_t nbyte) {
    return sos_write(buf, nbyte);
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

/**
 * Pack characters of the message into seL4_words
 * @param msgdata message to be printed
 * @param count length of the message
 * @returns the number of characters encoded
 */
static void encode_section(const char* msgdata, size_t count) {
    size_t mr_idx,i,j;
    seL4_Word pack;
    i = 0;
    mr_idx = PRINT_MESSAGE_START;
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

/**
 * Encode and send a section of message data over IPC for writing.  A section
 * is defined by the IPC message size limitations.
 * @param msgdata message to be printed
 * @param count length of the message.
 */
static size_t write_section(const char *msgdata, size_t count) {
    /* Arg count based on number of characters.  Rounded up. */
    int arg_num = ((count + sizeof(seL4_Word) - 1) >> 2) + PRINT_MESSAGE_START;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(0, 0, 0, arg_num);
    seL4_SetTag(tag);
    seL4_SetMR(0, SOS_SYSCALL_PRINT);
    seL4_SetMR(1, count);
    encode_section(msgdata, count);
    seL4_Call(SYSCALL_ENDPOINT_SLOT, tag);
    /* the reply - chars written */
    return seL4_GetMR(1);
}

/**
 * Write-out the provided message.
 * @param vData pointer to a character array
 * @param count length of the array
 */
size_t sos_write(void *vData, size_t count) {
    size_t i, seg_count;
    // The number of characters we can encode in a single IPC message
    size_t usable_msg_len = (seL4_MsgMaxLength - PRINT_MESSAGE_START) * sizeof(seL4_Word);
    char* msgdata = vData;
    size_t ipc_sections = count / usable_msg_len;
    size_t write_total = 0;
    for (i = 0; count && i <= ipc_sections; i++) {
        if (i == ipc_sections) {
            seg_count = count % usable_msg_len;
        } else {
            seg_count = usable_msg_len;
        }
        write_total += write_section(msgdata, seg_count);
        msgdata += seg_count;
    }
    return write_total;
}

size_t sos_read(void *vData, size_t count) {

    char *data = (char*)vData;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(seL4_NoFault, 0, 0, 2);
    seL4_SetTag(tag);
    seL4_SetMR(0, SOS_SYSCALL_READ);
    seL4_SetMR(1, count);
    
    seL4_MessageInfo_t reply = seL4_Call(SYSCALL_ENDPOINT_SLOT, tag);
   
    seL4_Word len = seL4_MessageInfo_get_length(reply);
    for (int i = 0; i < len ; i++) {
        data[i] = seL4_GetMR(i);
    }
    return len;
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
