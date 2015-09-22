#ifndef _SOS_SYSCALL_H_
#define _SOS_SYSCALL_H_

#include <sos.h>
#include <stdint.h>
#include <stdbool.h>
#include <serial/serial.h>
#include "addrspace.h"
#include "process.h"

typedef enum iop_direction {READ, WRITE, NONE} iop_direction_t;

size_t sys_print(size_t num_args);

int sos__sys_open(void);

int sos__sys_read(void);

int sos__sys_write(void);

int sos__sys_stat(void) ;

int sos__sys_getdirent(void);

int sos__sys_close(void);

int sos__sys_proc_create(void);

void ipc_read(int start, char *buf);

void iov_ensure_loaded(iovec_t* iov);

void syscall_end_continuation(sos_proc_t *proc, int retval, bool success);

iovec_t *cbuf_to_iov(client_vaddr buf, size_t nbyte, iop_direction_t dir);
void ipc_write(int start, char* msgdata, size_t length);
io_device_t* device_handler_str(const char* filename);

extern int pkg_size;
extern int pkg_num;
extern bool pkg_nfs;

#endif
