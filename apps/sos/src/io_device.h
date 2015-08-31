#ifndef _IO_DEVICE_H_
#define _IO_DEVICE_H_

#include <sos.h>
#include "serial.h"
#include "syscall.h"

typedef struct io_device {
    int (*open)(void);
    int (*close)(void);
    int (*read)(iovec_t*);
    int (*write)(iovec_t*);
} io_device_t;

typedef struct device_map {
    io_device_t *handler;
    char *name;
    int fd;
} device_map_t;

#define DEVICE_NUM 1


#endif
