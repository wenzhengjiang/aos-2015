/*
 * Copyright 2014, NICTA
 *
 * This software may be distributed and modified according to the terms of
 * the BSD 2-Clause license. Note that NO WARRANTY is provided.
 * See "LICENSE_BSD2.txt" for details.
 *
 * @TAG(NICTA_BSD)
 */

/****************************************************************************
 *
 *      $Id:  $
 *
 *      Description: Simple milestone 0 test.
 *
 *      Author:			Godfrey van der Linden
 *      Original Author:	Ben Leslie
 *
 ****************************************************************************/

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include <sel4/sel4.h>


#include "ttyout.h"

// Block a thread forever
// we do this by making an unimplemented system call.
static void
thread_block(void){
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(0, 0, 0, 1);
    seL4_SetTag(tag);
    seL4_SetMR(0, 1);
    seL4_Call(SYSCALL_ENDPOINT_SLOT, tag);
}

int main(void){
    ssize_t count = 0;
    const char* msg = "This is a message that's exactly 100 bytes long....................................................\n";
    do {
        count = printf("task:\tHello world, I'm\ttty_test!\n");
        printf("count written: %d\n", count);
        count = printf("Terminated early\n\0 DID NOT TERMINATE!\n");
        printf("count written: %d\n", count);
        count = printf("");
        printf("count written: %d\n", count);
        count = printf("%s%s", msg, msg);
        printf("count written: %d\n", count);
        count = printf("%s%s%s%s%s%s", msg, msg, msg, msg, msg, msg);
        printf("count written: %d\n", count);
        count = printf("\n");
        count = printf("1\n");
        count = printf("12\n");
        count = printf("123\n");
        count = printf("1234\n");
        count = printf("12345\n");
        count = printf("123456\n");
        count = printf("1234567\n");
        count = printf("12345678\n");
        count = printf("123456789\n");
        thread_block();
        // sleep(1);	// Implement this as a syscall
    } while(1);

    return 0;
}
