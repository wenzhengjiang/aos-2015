/*
 * Copyright 2014, NICTA
 *
 * This software may be distributed and modified according to the terms of
 * the BSD 2-Clause license. Note that NO WARRANTY is provided.
 * See "LICENSE_BSD2.txt" for details.
 *
 * @TAG(NICTA_BSD)
 */

#include <sel4/sel4.h>
#include <elf/elf.h>
#include <string.h>
#include <assert.h>
#include <cspace/cspace.h>
#include <errno.h>

#include "elf.h"
#include "process.h"
#include "addrspace.h"
#include "frametable.h"
#include "syscall.h"

#include <device/vmem_layout.h>
#include <ut/ut.h>
#include <device/mapping.h>

#define verbose 5
#include <log/debug.h>
#include <log/panic.h>

/* Minimum of two values. */
#define MIN(a,b) (((a)<(b))?(a):(b))

#define PAGESIZE              (1 << (seL4_PageBits))
#define PAGEMASK              ((PAGESIZE) - 1)
#define PAGE_ALIGN(addr)      ((addr) & ~(PAGEMASK))
#define IS_PAGESIZE_ALIGNED(addr) !((addr) &  (PAGEMASK))


extern seL4_ARM_PageDirectory dest_as;

/*
 * Convert ELF permissions into seL4 permissions.
 */
static inline seL4_Word get_sel4_rights_from_elf(unsigned long permissions) {
    seL4_Word result = 0;

    if (permissions & PF_R)
        result |= seL4_CanRead;
    if (permissions & PF_X)
        result |= seL4_CanRead;
    if (permissions & PF_W)
        result |= seL4_CanWrite;

    return result;
}

/*
 * Inject data into the given vspace.
 */
int load_page_into_vspace(sos_proc_t* proc,
                          seL4_ARM_PageDirectory dest_as,
                          uint32_t src,
                          unsigned long dst,
                          unsigned long permissions) {

    /* Overview of ELF segment loading

       dst: destination base virtual address of the segment being loaded
       segment_size: obvious
       
       So the segment range to "load" is [dst, dst + segment_size).

       The content to load is either zeros or the content of the ELF
       file itself, or both.

       The split between file content and zeros is a follows.

       File content: [dst, dst + file_size)
       Zeros:        [dst + file_size, dst + segment_size)

       Note: if file_size == segment_size, there is no zero-filled region.
       Note: if file_size == 0, the whole segment is just zero filled.

       The code below relies on seL4's frame allocator already
       zero-filling a newly allocated frame.

    */

    sos_addrspace_t *as = proc_as(proc);

    /* We work a page at a time in the destination vspace. */
    assert(as);
    assert((src == PAGE_ALIGN(src)) && (dst == PAGE_ALIGN(dst))); 
    dprintf(-1, "load_page_into_vspace: src=%08x,dst=%08x\n", src, dst);

    unsigned long kdst = as_lookup_sos_vaddr(as, dst);
    seL4_CPtr sos_cap = frame_cap(kdst);
    assert(sos_cap != seL4_CapNull);
    /* Now copy our data into the destination vspace. */
    int nbytes = PAGESIZE - (dst & PAGEMASK);

    proc->cont.iov = iov_create(dst, nbytes, NULL, NULL, false);
    if (!proc->cont.binary_nfs_read) {
        proc->cont.binary_nfs_read = true;
        proc->fd_table[BINARY_READ_FD]->io->read(proc->cont.iov,
                                                 proc->cont.fd,
                                                 nbytes);
    }

    /* Not observable to I-cache yet so flush the frame */
    seL4_ARM_Page_Unify_Instruction(sos_cap, 0, PAGESIZE);

    dprintf(-1, "load_page_into_vspace: finished\n");
    return 0;
}

int elf_load(sos_proc_t* proc, seL4_ARM_PageDirectory dest_as, char *elf_file) {

    int num_headers;
    int err;
    int i;

    /* Ensure that the ELF file looks sane. */
    if (elf_checkFile(elf_file)){
        return seL4_InvalidArgument;
    }

    num_headers = elf_getNumProgramHeaders(elf_file);
    sos_addrspace_t *as = proc_as(proc);

    for (i = 0; i < num_headers; i++) {
        uint64_t source_addr;
        unsigned long flags, segment_size, vaddr;

        /* Skip non-loadable segments (such as debugging data). */
        if (elf_getProgramHeaderType(elf_file, i) != PT_LOAD)
            continue;

        /* Fetch information about this segment. */
        source_addr = elf_getProgramHeaderOffset(elf_file, i);
        segment_size = elf_getProgramHeaderMemorySize(elf_file, i);
        vaddr = elf_getProgramHeaderVaddr(elf_file, i);
        flags = elf_getProgramHeaderFlags(elf_file, i);
        /* Copy it across into the vspace. */
        dprintf(-1, " * Loading segment %08x-->%08x\n", (int)vaddr, (int)(vaddr + segment_size));
        sos_region_t* reg = as_region_create(as, (seL4_Word)vaddr, ((seL4_Word)vaddr + segment_size), (int)(get_sel4_rights_from_elf(flags) & seL4_AllRights), source_addr);
        // err = load_segment_into_vspace(proc, dest_as, source_addr, segment_size, file_size, vaddr,
        if (reg == NULL) {
            return ENOMEM;
        }
    }
    dprintf(2, "elf_load finished");
    return 0;
}
