/*
 * Copyright 2014, NICTA
 *
 * This software may be distributed and modified according to the terms of
 * the BSD 2-Clause license. Note that NO WARRANTY is provided.
 * See "LICENSE_BSD2.txt" for details.
 *
 * @TAG(NICTA_BSD)
 */

#ifndef _LIBDEVICE_ELF_H_
#define _LIBDEVICE_ELF_H_

#include <sel4/sel4.h>
#include "addrspace.h"

int elf_load(sos_addrspace_t* as, seL4_ARM_PageDirectory dest_pd, char* elf_file);

#endif
