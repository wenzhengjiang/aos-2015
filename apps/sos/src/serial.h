#ifndef _SERIAL_H_
#define _SERIAL_H_

#include <stdint.h>
#include "addrspace.h"

void serial_open(void);
int serial_read(sos_vaddr buf, size_t nbyte);
int serial_write(sos_vaddr buf, size_t nbyte);


#endif
