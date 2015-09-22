#include <assert.h>
#include <log/panic.h>
#include <string.h>
#include <errno.h>

#include "file.h"
#include "process.h"
#include "frametable.h"
#include "serial.h"

#define FD_TABLE_SIZE (MAX_FD+1)
#define OPEN_FILE_TABLE_SIZE 1024

static of_entry_t of_table[OPEN_FILE_TABLE_SIZE];

of_entry_t* get_ofe() {
    for (int i = 0; i < OPEN_FILE_TABLE_SIZE; i++) {
        if (of_table[i].io == NULL) {
            return &of_table[i];
        }
    }
    return NULL;
}

static int fd_create2(fd_table_t fdt, fhandle_t* handle, io_device_t* io, fmode_t mode, int i) {
    assert (fdt[i] == NULL);

    fdt[i] = get_ofe();
    if (fdt[i] == NULL) return -1;
    fdt[i]->offset = 0;
    fdt[i]->mode = mode;
    fdt[i]->fhandle = handle;
    fdt[i]->io = io;
    return 0;
}

// return fd; -1 means ENOMEM
int fd_create(fd_table_t fdt, fhandle_t* handle, io_device_t* io, fmode_t mode) {
    assert(fdt);
    assert(handle || io);
    for (int i = 0; i < FD_TABLE_SIZE; i++) {
        if (fdt[i] == NULL)  {
            fdt[i] = get_ofe();
            if (fdt[i] == NULL) return -1;
            fdt[i]->offset = 0;
            fdt[i]->mode = mode;
            fdt[i]->fhandle = handle;
            fdt[i]->io = io;
            return i;
        }
    }
    return -1;
}

int fd_free(fd_table_t fd_table, int fd) {
    assert(fd_table[fd]);
    if (fd_table[fd] == NULL) {
        printf("fd %d not found to close\n", fd);
        return -1;
    }
    fd_table[fd]->io = NULL;
    fd_table[fd] = NULL;
    if(fd_table[fd]->fhandle)
        free(fd_table[fd]->fhandle);
    return 0;
}

int init_fd_table(fd_table_t *fd_table) {
    frame_alloc((seL4_Word*)fd_table);

    conditional_panic(!fd_table, "No memory for new TCB");
    if(fd_create2(*fd_table, 0, &serial_io, FM_READ,1) < 0) return ENOMEM;
    if(fd_create2(*fd_table, 0, &serial_io, FM_READ,2) < 0) return ENOMEM;

    return 0;
}
