/*
 * Copyright 2014, NICTA
 *
 * This software may be distributed and modified according to the terms of
 * the BSD 2-Clause license. Note that NO WARRANTY is provided.
 * See "LICENSE_BSD2.txt" for details.
 *
 * @TAG(NICTA_BSD)
 */

#include <autoconf.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <sys/mman.h>
#include <errno.h>
#include <assert.h>

#include <proc/addrspace.h>
#include <proc/process.h>

/*
 * Statically allocated morecore area.
 *
 * This is rather terrible, but is the simplest option without a
 * huge amount of infrastructure.
 */
#define MORECORE_AREA_BYTE_SIZE 0x100000
char morecore_area[MORECORE_AREA_BYTE_SIZE];

/* Pointer to free space in the morecore area. */
static uintptr_t morecore_base = (uintptr_t) &morecore_area;
static uintptr_t morecore_top = (uintptr_t) &morecore_area[MORECORE_AREA_BYTE_SIZE];

/* Actual morecore implementation
   returns 0 if failure, returns newbrk if success.
*/

long
sys_brk(va_list ap)
{

    uintptr_t ret;
    uintptr_t newbrk = va_arg(ap, uintptr_t);

    sos_proc_t* proc = current_process();
    assert(proc);
    sos_addrspace_t* as = proc_as(proc);
    assert(as);
    /*if the newbrk is 0, return the bottom of the heap*/
    if (!newbrk) {
        ret = as->heap_region->end;
    } else if (newbrk < as->stack_region->start && newbrk > as->heap_region->start) {
        ret = as->heap_region->end = newbrk;
    } else {
        ret = 0;
    }

    return ret;
}

/* Large mallocs will result in muslc calling mmap, so we do a minimal implementation
   here to support that. We make a bunch of assumptions in the process */
long
sys_mmap2(va_list ap)
{
    void *addr = va_arg(ap, void*);
    size_t length = va_arg(ap, size_t);
    int prot = va_arg(ap, int);
    int flags = va_arg(ap, int);
    int fd = va_arg(ap, int);
    off_t offset = va_arg(ap, off_t);
    (void)addr;
    (void)prot;
    (void)fd;
    (void)offset;
    if (flags & MAP_ANONYMOUS) {
        /* Steal from the top */
        uintptr_t base = morecore_top - length;
        if (base < morecore_base) {
            return -ENOMEM;
        }
        morecore_top = base;
        return base;
    }
    assert(!"not implemented");
    return -ENOMEM;
}

long
sys_mremap(va_list ap)
{
    assert(!"not implemented");
    return -ENOMEM;
}
