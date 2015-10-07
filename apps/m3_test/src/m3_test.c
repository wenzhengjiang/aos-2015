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
#include <string.h>
#include <stdlib.h>
#include <syscall.h>
#include <sos.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include "m3_test.h"


#include <sel4/sel4.h>

#define NPAGES 27

/* called from pt_test */
static void
do_pt_test( char *buf )
{
    int i;

    /* set */
    for(i = 0; i < NPAGES; i ++) {
        printf("setting : %p\n", &buf[i * 4096]);
	    buf[i * 4096] = i;
    }

    /* check */
    for(i = 0; i < NPAGES; i ++) {
        printf("checking : %p\n", &buf[i * 4096]);
	    assert(buf[i * 4096] == i);
    }
}

static void
pt_test( void )
{
    /* need a decent sized stack */
    char buf1[NPAGES * 4096], *buf2 = NULL;

    /* check the stack is above phys mem */
    assert((void *) buf1 > (void *) 0x20000000);

    /* stack test */
    printf("m3_test: start stack test ...\n");
    do_pt_test(buf1);
    //printf("STATIC TESTS PASSED\n");
    /* heap test */
    buf2 = malloc(NPAGES * 4096);
    assert(buf2);
    printf("m3_test: start heap test ...\n");
    do_pt_test(buf2);
    free(buf2);
    //printf("DYNAMIC TESTS PASSED\n");
}

static void
thread_block(void){
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(0, 0, 0, 1);
    seL4_SetTag(tag);
    seL4_SetMR(0, 1);
    seL4_Call(SYSCALL_ENDPOINT_SLOT, tag);
}

static void cp_test(void) {
    int fd, fd_out;
    char *file1, *file2;
    size_t buf_size = 1024 * 5;
    char *really_big_buf = malloc(buf_size);
    assert(really_big_buf);
    memset(really_big_buf, 0, buf_size);

    int num_read, num_written = 0;

    pid_t pid = sos_my_id();
    file1 = "bootimg.elf";
    file2 = "bootimgx.elf";
    fd = open(file1, O_RDONLY);
    fd_out = open(file2, O_WRONLY);

    assert(fd >= 0);
    printf("\n\n=== COPYING START %d ===\n", pid);
    int cnt =  0;
    while ((num_read = read(fd, really_big_buf, buf_size)) > 0) {
        num_written = write(fd_out, really_big_buf, num_read);
        printf("proc %d - %d\n", pid, cnt++);
    }

    if (num_read == -1 || num_written == -1) {
        printf("error on cp %d, %d\n", num_read, num_written);
    }
    printf("\n\n=== COPYING END ===\n");
   
}
int main(void){
    //pt_test();
    cp_test();
    return 0;
}
