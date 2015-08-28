#ifndef _SOS_SYSCALL_H_
#define _SOS_SYSCALL_H_

#include <stdint.h>
#include "process.h"
#include "addrspace.h"

void sys_notify_client(uint32_t id, void *data);
void sys_serial_handler(struct serial *serial, char c);
void sys_serial_open(void);
int sys_serial_read(sos_vaddr buf, size_t nbyte);
int sys_serial_write(sos_vaddr buf, size_t nbyte);
size_t sys_print(size_t num_args);

#endif
