#ifndef _HANDLER_H_
#define _HANDLER_H_

#include <sel4/sel4.h>

typedef void (*syscall_handler)(seL4_CPtr reply_cap);

typedef syscall_event {
    void (hander*) handler (seL4_CPtr reply_cap);
} syscall_event_t;

void register_handlers(void) ;

void handle_syscall(seL4_Word badge, int num_args);

#endif
