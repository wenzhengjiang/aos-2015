#ifndef _IO_DEVICE_H_
#define _IO_DEVICE_H_

#include <sos.h>
#include "syscall.h"

typedef struct io_device {
    int (*open)(char* pathname, fmode_t mode);
    int (*close)(void);
    int (*read)(iovec_t*);
    int (*write)(iovec_t*);
} io_device_t;

#endif
