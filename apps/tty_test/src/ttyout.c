/*
 * Copyright 2014, NICTA
 *
 * This software may be distributed and modified according to the terms of
 * the BSD 2-Clause license. Note that NO WARRANTY is provided.
 * See "LICENSE_BSD2.txt" for details.
 *
 * @TAG(NICTA_BSD)
 */

/****************************************************************************
 *
 *      $Id:  $
 *
 *      Description: Simple milestone 0 code.
 *      		     Libc will need sos_write & sos_read implemented.
 *
 *      Author:      Ben Leslie
 *
 ****************************************************************************/

#include <stdarg.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "ttyout.h"

#include <sel4/sel4.h>

/* TODO: This should be centralised.  But it seems wrong to depend on a sos
 * header and is not clear yet how to structure any library created.  So this
 * is duplicated for now. */
#define SOS_SYSCALL_PRINT 2
#define PRINT_MESSAGE_START 2

static size_t sos_debug_print(const void *vData, size_t count) {
    size_t i;
    const char *realdata = vData;
    for (i = 0; i < count; i++) {
        seL4_DebugPutChar(realdata[i]);
    }
    return count;
}

/**
 * Pack characters of the message into seL4_words
 * @param msgdata message to be printed
 * @param count length of the message
 * @returns the number of characters encoded
 */
void encode_section(const char* msgdata, size_t count) {
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
