#ifndef _HANDLER_H_
#define _HANDLER_H_

#include <sel4/sel4.h>

void register_handlers(void) ;

void handle_syscall(seL4_Word badge, int num_args);

int sos_vm_fault(seL4_Word read_fault, seL4_Word faultaddr);

#endif
