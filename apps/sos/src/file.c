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

int fd_create_fd(fd_table_t fdt, fhandle_t* handle, io_device_t* io, fmode_t mode, int i) {
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
    if(fd_table[fd]->fhandle)
        free(fd_table[fd]->fhandle);
    fd_table[fd] = NULL;
    return 0;
}

int free_fd_table(fd_table_t fdt) {
    for (int i = 0; i < FD_TABLE_SIZE; i++) {
        if (fdt[i] != NULL)  {
            fd_free(fdt, i);
        }
    }
    sos_unmap_frame((seL4_Word)fdt);
    return 0;
}
