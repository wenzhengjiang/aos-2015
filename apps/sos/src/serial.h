#ifndef _SERIAL_H_
#define _SERIAL_H_

#include <stdint.h>
#include "addrspace.h"
#include "syscall.h"

int sos_serial_open(const char* filename, fmode_t mode);

int sos_serial_read(iovec_t* vecs, int fd, int count);

int sos_serial_write(iovec_t* vec, int fd, int count);

int sos_serial_close(int fd);

void sos_serial_init(void);

extern io_device_t serial_io;

#endif
