#include <assert.h>
#include <log/panic.h>

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



int init_fd_table(void) {
    sos_proc_t* proc = current_process();
    frame_alloc((seL4_Word*)&proc->fd_table);
    conditional_panic(!proc->fd_table, "No memory for new TCB");
    assert(fd_create(proc->fd_table, 0, &serial_io, FM_READ) == 0);
    assert(fd_create(proc->fd_table, 0, &serial_io, FM_WRITE) == 1);
    assert(fd_create(proc->fd_table, 0, &serial_io, FM_WRITE) == 2);

    return 0;
}
