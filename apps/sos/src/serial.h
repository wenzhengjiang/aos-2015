#ifndef _SERIAL_H_
#define _SERIAL_H_

#include <stdint.h>
#include "addrspace.h"
#include "syscall.h"
#include "io_device.h"

int sos_serial_open(char* pathname, fmode_t mode);

int sos_serial_read(iovec_t* vecs);

int sos_serial_write(iovec_t* vecs);

int sos_serial_close(void);

extern io_device_t serial_io;

#endif
