#ifndef _M3_TEST_H
#define _M3_TEST_H

#include <stdio.h>
#define SYSCALL_ENDPOINT_SLOT  (1)
extern size_t
sos_write(void *data, size_t count);
extern size_t
sos_read(void *data, size_t count);

#endif
