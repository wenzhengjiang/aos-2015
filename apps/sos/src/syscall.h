#ifndef _SOS_SYSCALL_H_
#define _SOS_SYSCALL_H_

#include <sos.h>
#include <stdint.h>
#include <stdbool.h>
#include <serial/serial.h>
#include "addrspace.h"
#include "process.h"

size_t sys_print(size_t num_args);

int sos__sys_open(void);

int sos__sys_read(void);

int sos__sys_write(void);

int sos__sys_stat(void) ;

int sos__sys_getdirent(void);

int sos__sys_close(void);

void ipc_read(int start, char *buf);

void syscall_end_continuation(sos_proc_t *proc, int retval, bool success);

extern int pkg_size;
extern int pkg_num;
extern bool pkg_nfs;

#endif
