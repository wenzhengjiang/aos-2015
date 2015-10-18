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
#include <setjmp.h>

#include "elf.h"
#include "process.h"
#include "addrspace.h"
#include "frametable.h"
#include "syscall.h"

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


extern jmp_buf ipc_event_env;

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

/**
 * @brief load a page from elf file
 *
 * @param proc the client which is loading page
 * @param src offset of elf file of the loading page
 * @param dst destination base virtual address of the page being loaded
 *
 * @return 
 */
int load_page_into_vspace(sos_proc_t* proc,
                          uint32_t offset,
                          unsigned long faultdst) {

    sos_addrspace_t *as = proc_as(proc);

    /* We work a page at a time in the destination vspace. */
    assert(as);
    //assert(dst == PAGE_ALIGN(dst));
    unsigned long dst = faultdst;
    sos_region_t *reg = as_vaddr_region(as, dst);
    assert(reg);
    size_t nbytes = PAGESIZE;


    if (reg->start > dst) {
        nbytes -= (reg->start - dst);
        dst = reg->start;
    } else {
        dst = PAGE_ALIGN(dst);
    }

    if ((reg->end - dst) < PAGESIZE) {
        nbytes -= PAGESIZE - (reg->end % PAGESIZE);
    }

    unsigned long src = offset + dst - reg->start;
    dprintf(3, "load_page_into_vspace: src=%08x,dst=%08x\n", src, dst);

    proc->cont.iov = iov_create(dst, nbytes, NULL, NULL, false);
    assert(proc->fd_table[proc->cont.fd]);
    proc->fd_table[proc->cont.fd]->offset = src;

    if (!proc->cont.binary_nfs_read) {
        proc->cont.binary_nfs_read = true;
        proc->fd_table[BINARY_READ_FD]->io->read(proc->cont.iov,
                                                 proc->cont.fd,
                                                 nbytes);
        longjmp(ipc_event_env, -1);
    }

    unsigned long kdst = as_lookup_sos_vaddr(as, dst);
    if (kdst == 0) {
        return EINVAL;
    }
    seL4_CPtr sos_cap = frame_cap(kdst);
    if (sos_cap == seL4_CapNull) {
        return EINVAL;
    }
    assert(sos_cap != seL4_CapNull);

    /* Not observable to I-cache yet so flush the frame */
    seL4_ARM_Page_Unify_Instruction(sos_cap, 0, PAGESIZE);

    dprintf(3, "load_page_into_vspace: finished\n");
    return 0;
}

int elf_load(sos_proc_t* proc, char *elf_file) {

    /* Ensure that the ELF file looks sane. */
    if (elf_checkFile(elf_file)){
        ERR("Invalid elf file\n");
        return seL4_InvalidArgument;
    }

    int num_headers = elf_getNumProgramHeaders(elf_file);
    sos_addrspace_t *as = proc_as(proc);
    if (num_headers * sizeof(struct Elf32_Phdr) + sizeof(struct Elf32_Header) > PAGESIZE) {
        ERR("Too many ELF segments in file\n");
    }
    for (int i = 0; i < num_headers; i++) {

        seL4_Word source_addr;
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
        dprintf(3, " * Loading segment %08x-->%08x %x\n", (int)vaddr, (int)(vaddr + segment_size), (int)(get_sel4_rights_from_elf(flags) & seL4_AllRights));
        sos_region_t* reg = as_region_create(as, (seL4_Word)vaddr, ((seL4_Word)vaddr + segment_size), (int)(get_sel4_rights_from_elf(flags) & seL4_AllRights), source_addr);
        // err = load_segment_into_vspace(proc, dest_as, source_addr, segment_size, file_size, vaddr,
        if (reg == NULL) {
            ERR("no reg\n");
            return ENOMEM;
        }
    }
    dprintf(2, "elf_load finished");
    return 0;
}
