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

int main(void){
    size_t count = 0;
    count = printf("task:\tHello world, I'm\ttty_test!\n");
    printf("count written: %d\n", count);
    count = printf("Terminated early\nDID NOT TERMINATE!\n");
    printf("count written: %d\n", count);
    count = printf("1");
    printf("count written: %d\n", count);
    printf("count written: %d\n", count);
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
    return 0;
}
