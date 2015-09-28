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

#include "elf.h"
#include "process.h"
#include "addrspace.h"
#include "frametable.h"

#include <device/vmem_layout.h>
#include <ut/ut.h>
#include <device/mapping.h>

#define verbose 0
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
static int load_segment_into_vspace(sos_proc_t* proc,
                                    seL4_ARM_PageDirectory dest_as,
                                    char *src, unsigned long segment_size,
                                    unsigned long file_size, unsigned long dst,
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
    assert(file_size <= segment_size);

    /* We work a page at a time in the destination vspace. */
    assert(as);

    sos_region_t *reg = NULL;
    reg = as_vaddr_region(as, dst);
    if (proc->cont.elf_segment_pos == 0) {
        if (reg == NULL) {
            reg = as_region_create(as, (seL4_Word)dst, ((seL4_Word)dst + segment_size), (int)permissions);
        }
    }

    if (reg == NULL) {
        // failed to create a region
        return 1;
    }

    dst += proc->cont.elf_segment_pos;
    src += proc->cont.elf_segment_pos;

    while(proc->cont.elf_segment_pos < segment_size) {
        seL4_Word paddr;
        seL4_CPtr sos_cap;
        seL4_Word vpage, kvpage;
        unsigned long kdst;
        int nbytes;
        vpage  = PAGE_ALIGN(dst);
        kvpage = PAGE_ALIGN(kdst);

        dprintf(-1, "load_segment_into_vspace: src=%08x,dst=%08x\n", src, dst);

        /* First we need to create a frame */
        as_create_page(as, vpage, permissions);

        kdst   = as_lookup_sos_vaddr(as, dst);
        sos_cap = frame_cap(kdst);
        assert(sos_cap != seL4_CapNull);
        /* Now copy our data into the destination vspace. */
        nbytes = PAGESIZE - (dst & PAGEMASK);
        if (proc->cont.elf_segment_pos < file_size){
            memcpy((void*)kdst, (void*)src, MIN(nbytes, file_size - proc->cont.elf_segment_pos));
        }

        /* Not observable to I-cache yet so flush the frame */
        seL4_ARM_Page_Unify_Instruction(sos_cap, 0, PAGESIZE);

        proc->cont.elf_segment_pos += nbytes;
        dst += nbytes;
        src += nbytes;
        
    }
    dprintf(-1, "load_segment_into_vspace: finished\n");
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
    for (i = proc->cont.elf_header; i < num_headers; i++) {
        proc->cont.elf_header = i;
        char *source_addr;
        unsigned long flags, file_size, segment_size, vaddr;

        /* Skip non-loadable segments (such as debugging data). */
        if (elf_getProgramHeaderType(elf_file, i) != PT_LOAD)
            continue;

        /* Fetch information about this segment. */
        source_addr = elf_file + elf_getProgramHeaderOffset(elf_file, i);
        file_size = elf_getProgramHeaderFileSize(elf_file, i);
        segment_size = elf_getProgramHeaderMemorySize(elf_file, i);
        vaddr = elf_getProgramHeaderVaddr(elf_file, i);
        flags = elf_getProgramHeaderFlags(elf_file, i);
        proc->status.size += segment_size;
        /* Copy it across into the vspace. */
        dprintf(-1, " * Loading segment %08x-->%08x\n", (int)vaddr, (int)(vaddr + segment_size));
        err = load_segment_into_vspace(proc, dest_as, source_addr, segment_size, file_size, vaddr,
                                       get_sel4_rights_from_elf(flags) & seL4_AllRights);
        conditional_panic(err != 0, "Elf loading failed!\n");
        proc->cont.elf_segment_pos = 0;
    }

    return 0;
}
