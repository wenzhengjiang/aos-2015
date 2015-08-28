#ifndef _SERIAL_H_
#define _SERIAL_H_

#include <stdint.h>
#include "addrspace.h"
#include "syscall.h"

void sos_serial_open(void);

int sos_serial_read(iovec_t* vecs);

int sos_serial_write(iovec_t* vecs);


#endif
