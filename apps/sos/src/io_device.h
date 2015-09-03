#ifndef _IO_DEVICE_H_
#define _IO_DEVICE_H_

#include <sos.h>
#include "serial.h"
#include "syscall.h"

typedef struct io_device {
    int (*open)(char* path, fmode_t mode);
    int (*close)(int fh);
    int (*read)(iovec_t*, int fh);
    int (*write)(iovec_t*, int fh);
    int (*getattr)(iovec_t*, int fh);
    int (*readdir)(iovec_t*, int fh);
} io_device_t;

typedef struct device_map {
    io_device_t *handler;
    char *name;
    int fd;
} device_map_t;

#define DEVICE_NUM 1

#endif
