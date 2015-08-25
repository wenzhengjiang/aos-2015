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
#include <syscall.h>
#include "m3_test.h"

#include <sel4/sel4.h>

#define NPAGES 27

/* called from pt_test */
static void
do_pt_test( char *buf )
{
    int i;

    /* set */
    for(i = 0; i < NPAGES; i ++)
	buf[i * 4096] = i;

    /* check */
    for(i = 0; i < NPAGES; i ++)
	assert(buf[i * 4096] == i);
}

static void
pt_test( void )
{
    /* need a decent sized stack */
    char buf1[NPAGES * 4096], *buf2 = NULL;

    /* check the stack is above phys mem */
    assert((void *) buf1 > (void *) 0x20000000);

    /* stack test */
    do_pt_test(buf1);
    printf("STATIC TESTS PASSED\n");
    /* heap test */
    buf2 = malloc(NPAGES * 4096);
    assert(buf2);
    do_pt_test(buf2);
    free(buf2);
    printf("DYNAMIC TESTS PASSED\n");
}

static void
thread_block(void){
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(0, 0, 0, 1);
    seL4_SetTag(tag);
    seL4_SetMR(0, 1);
    seL4_Call(SYSCALL_ENDPOINT_SLOT, tag);
}

int main(void){
    do {
        printf("M3 TEST\n");
        pt_test();
        thread_block();
        // sleep(1);	// Implement this as a syscall
    } while(1);
    return 0;
}
