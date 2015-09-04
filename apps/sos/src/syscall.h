#ifndef _SOS_SYSCALL_H_
#define _SOS_SYSCALL_H_

#include <sos.h>
#include <stdint.h>
#include <serial/serial.h>
#include "addrspace.h"
#include "process.h"

size_t sys_print(size_t num_args);

int sos__sys_open(const char *path, fmode_t mode);

int sos__sys_read(int file, client_vaddr buf, size_t nbyte);

int sos__sys_write(int file, client_vaddr buf, size_t nbyte);

int sos__sys_stat(char *path, client_vaddr buf) ;

int sos__sys_getdirent(int pos, client_vaddr name, size_t nbyte);

void ipc_read(int start, char *buf);


void syscall_end_continuation(sos_proc_t *proc, int retval);

#endif
