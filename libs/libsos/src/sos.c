/*
 * Copyright 2014, NICTA
 *
 * This software may be distributed and modified according to the terms of
 * the BSD 2-Clause license. Note that NO WARRANTY is provided.
 * See "LICENSE_BSD2.txt" for details.
 *
 * @TAG(NICTA_BSD)
 */

/*Implementation of sos interface*/

#include <stdarg.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <sos.h>
#include <stdio.h>
#include <fcntl.h>
#include <syscallno.h>
#include <sel4/sel4.h>

#define verbose 5
#include <log/debug.h>
#include <log/panic.h>

/**
 * @brief Writing a string message to IPC buffer by packing them to seL4_Words
 *
 * @param start position where the message is inserted
 * @param msgdata bytes need to be packed
 */
static void ipc_write_str(int start, const char* msgdata) {
    size_t mr_idx,i,j;
    seL4_Word pack;
    i = 0;
    mr_idx = start;
    int count = strlen(msgdata);
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
    seL4_SetMR(mr_idx, 0);
}

/**
 * @brief Unpack a seL4_Word to 4 bytes
 *
 * @param msgBuf buffer to store unpacked bytes
 * @param packed_data seL4_Word needs to be unpacked
 *
 */
static void unpack_word_bin(char* msgBuf, seL4_Word packed_data) {
    int j = sizeof(seL4_Word);
    while (j > 0) {
        // Unpack data encoded 4-chars per word.
        *msgBuf = (char)(packed_data >> ((--j) * 8));
        msgBuf++;
    }
}

static void ipc_read_bin(int start, char *buf) {
    assert(buf && start > 0);
    size_t length = seL4_GetMR(start);
    int k = 0, i;
    for (i = start + 1; i < seL4_MsgMaxLength && k < length; i++) {
       unpack_word_bin(buf+k, seL4_GetMR(i));
       k += sizeof(seL4_Word);
    }
}

fmode_t mode2fmode(mode_t mode) {
    fmode_t ret = 0;
    if (mode == 0 || (mode & O_RDWR)) {
        ret |= FM_READ;
    }
    if ((mode & O_WRONLY) || (mode & O_RDWR)) {
        ret |= FM_WRITE;
    }
    return ret;
}

int sos_sys_open(const char *path, fmode_t mode) {
    int len = ((strlen(path)+1) + sizeof(seL4_Word)-1) >> 2;
    mode = mode2fmode(mode);
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(seL4_NoFault, 0, 0, 2 + len);
    seL4_SetTag(tag);
    seL4_SetMR(0, (seL4_Word)SOS_SYSCALL_OPEN);
    seL4_SetMR(1, (seL4_Word)mode);
    ipc_write_str(OPEN_MESSAGE_START, path);
    char data[100];
    sprintf(data, "sys_open %x %x %x %x\n", seL4_GetMR(2), seL4_GetMR(3), seL4_GetMR(4));
    seL4_MessageInfo_t reply = seL4_Call(SYSCALL_ENDPOINT_SLOT, tag);
    if (seL4_MessageInfo_get_label(reply) != seL4_NoFault)
        return -1;
    else 
        return seL4_GetMR(0);
}

int sos_sys_read(int file, char *buf, size_t nbyte) {
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
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(seL4_NoFault, 0, 0, 4);
    seL4_SetTag(tag);
    seL4_SetMR(0, (seL4_Word)SOS_SYSCALL_WRITE); 
    seL4_SetMR(1, (seL4_Word)file);
    seL4_SetMR(2, (seL4_Word)buf);
    seL4_SetMR(3, (seL4_Word)nbyte);
    seL4_MessageInfo_t reply = seL4_Call(SYSCALL_ENDPOINT_SLOT, tag);
    //if (nbyte == 0) while (1);
    if (seL4_MessageInfo_get_label(reply) != seL4_NoFault)
        return 0;
    else 
        return seL4_GetMR(0);
}

void sos_sys_usleep(int msec) {
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(seL4_NoFault, 0, 0, 2);
    seL4_SetTag(tag);
    seL4_SetMR(0, SOS_SYSCALL_USLEEP); 
    seL4_SetMR(1, msec); 
    seL4_MessageInfo_t reply = seL4_Call(SYSCALL_ENDPOINT_SLOT, tag);
    assert(seL4_MessageInfo_get_label(reply) == seL4_NoFault);
}


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


int sos_stat(const char *path, sos_stat_t *buf) {
    if (path == NULL) {
        return -1;
    }
    int len = ((strlen(path)+1) + sizeof(seL4_Word)-1) >> 2;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(seL4_NoFault, 0, 0, 2+len);
    seL4_SetTag(tag);
    seL4_SetMR(0, SOS_SYSCALL_STAT);
    seL4_SetMR(1, (seL4_Word)buf);
    ipc_write_str(STAT_MESSAGE_START, path);
    seL4_MessageInfo_t reply = seL4_Call(SYSCALL_ENDPOINT_SLOT, tag);
    if(seL4_MessageInfo_get_label(reply) == seL4_NoFault) {
        ipc_read_bin(1, (char*)buf);
        return 0;
    } else {
        return -1;
    }
}

int sos_getdirent(int pos, char *name, size_t nbyte) {
    if (nbyte == 0) return 0;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(seL4_NoFault, 0, 0, 3);
    seL4_SetTag(tag);
    seL4_SetMR(0, SOS_SYSCALL_GETDIRENT); 
    seL4_SetMR(1, pos + 1); // in sos, index starts from 1 
    seL4_SetMR(2, nbyte-1); 
    seL4_MessageInfo_t reply = seL4_Call(SYSCALL_ENDPOINT_SLOT, tag);
    if(seL4_MessageInfo_get_label(reply) == seL4_NoFault) {
        ipc_read_bin(1, (char*)name);
        name[nbyte - 1] = 0;
        return seL4_GetMR(0);
    } else {
        return -1;
    }
}

size_t sos_write(void *data, size_t count) {
    return sos_sys_write(STDOUT_FD , (char*)data, count);
}

size_t sos_read(void *vData, size_t count) {
    return sos_sys_read(STDIN_FD, vData, count);
}

pid_t sos_process_create(const char *path) {

    int len = ((strlen(path)+1) + sizeof(seL4_Word)-1) >> 2;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(seL4_NoFault, 0, 0, 2 + len);
    seL4_SetTag(tag);
    seL4_SetMR(0, (seL4_Word)SOS_SYSCALL_PROC_CREATE);
    ipc_write_str(PROC_CREATE_MESSAGE_START, path);

    seL4_MessageInfo_t reply = seL4_Call(SYSCALL_ENDPOINT_SLOT, tag);
    if (seL4_MessageInfo_get_label(reply) != seL4_NoFault)
        return -1;
    else 
        return seL4_GetMR(0);
}

int sos_process_status(sos_process_t *processes, unsigned max) {
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(seL4_NoFault, 0, 0, 3);
    seL4_SetTag(tag);
    seL4_SetMR(0, SOS_SYSCALL_PROC_STATUS);
    seL4_SetMR(1, (seL4_Word)processes);
    seL4_SetMR(2, (seL4_Word)max);
    seL4_MessageInfo_t reply = seL4_Call(SYSCALL_ENDPOINT_SLOT, tag);
    if (seL4_MessageInfo_get_label(reply) != seL4_NoFault)
        return -1;
    else 
        return seL4_GetMR(0);
}

pid_t sos_process_wait(pid_t pid) {
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(seL4_NoFault, 0, 0, 2);
    seL4_SetTag(tag);
    seL4_SetMR(0, SOS_SYSCALL_WAITPID);
    seL4_SetMR(1, (seL4_Word)pid);
    seL4_MessageInfo_t reply = seL4_Call(SYSCALL_ENDPOINT_SLOT, tag);
    if (seL4_MessageInfo_get_label(reply) != seL4_NoFault)
        return -1;
    else 
        return seL4_GetMR(0);
}

int sos_process_delete(pid_t pid) {
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(seL4_NoFault, 0, 0, 2);
    seL4_SetTag(tag);
    seL4_SetMR(0, SOS_SYSCALL_PROC_DELETE);
    seL4_SetMR(1, (seL4_Word)pid);
    seL4_MessageInfo_t reply = seL4_Call(SYSCALL_ENDPOINT_SLOT, tag);
    if (seL4_MessageInfo_get_label(reply) != seL4_NoFault)
        return -1;
    else 
        return seL4_GetMR(0);
}

pid_t sos_my_id(void) {
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(seL4_NoFault, 0, 0, 1);
    seL4_SetTag(tag);
    seL4_SetMR(0, SOS_SYSCALL_GETPID);
    seL4_MessageInfo_t reply = seL4_Call(SYSCALL_ENDPOINT_SLOT, tag);
    if (seL4_MessageInfo_get_label(reply) != seL4_NoFault)
        return -1;
    else 
        return seL4_GetMR(0);
}

int sos_sys_close(int file) {
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(seL4_NoFault, 0, 0, 2);
    seL4_SetTag(tag);
    seL4_SetMR(0, SOS_SYSCALL_CLOSE);
    seL4_SetMR(1, (seL4_Word)file);
    seL4_MessageInfo_t reply = seL4_Call(SYSCALL_ENDPOINT_SLOT, tag);
    if(seL4_MessageInfo_get_label(reply) == seL4_NoFault) {
        return (int)seL4_GetMR(0);
    } else {
        return -1;
    }
}


