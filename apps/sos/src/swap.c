#include <setjmp.h>
#include <stdio.h>
#include <assert.h>

#include "swap.h"

static jmp_buf env;

static int j = 5;

static __attribute__((noreturn))
int test_longjmp(void)  {
    j = 10;
    longjmp(env, 30);
}

void test_setjmp(void) {
    printf("called test_setjmp\n");
    int i = 10;
    int res = setjmp(env);
    if (res != 0) {
        assert(res == 30);
        printf("CALLED LONGJMP OKAY\n");
        assert(j * i == 100);
        return;
    }
    test_longjmp();
}
