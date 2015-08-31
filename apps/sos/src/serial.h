#ifndef _SERIAL_H_
#define _SERIAL_H_

#include <stdint.h>
#include "addrspace.h"
#include "syscall.h"
#include "io_device.h"

#define SERIAL_FD 5
int sos_serial_open(void);

int sos_serial_read(iovec_t* vecs);

int sos_serial_write(iovec_t* vecs);

int sos_serial_close(void);

extern io_device_t serial_io;

#endif
