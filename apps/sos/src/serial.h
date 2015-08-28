#ifndef _SERIAL_H_
#define _SERIAL_H_

#include <stdint.h>
#include "addrspace.h"
#include "syscall.h"

void serial_open(void);

int serial_read(iovec_t* vecs, size_t nbyte);

int serial_write(iovec_t* vecs, size_t nbyte);


#endif
