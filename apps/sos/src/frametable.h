#ifndef _FRAME_TABLE_H_
#define _FRAME_TABLE_H_

#include <sel4/sel4.h>
#include <stdbool.h>

/* Maximum number of frames which will fit in our region */
#define SMALL_FT
#ifdef SMALL_FT
  #define MAX_FRAMES 1050
#else
  #define MAX_FRAMES ((PROCESS_STACK_TOP - FRAME_VSTART - PAGE_SIZE) / PAGE_SIZE)
#endif

void frame_init(void);
seL4_Word frame_alloc(seL4_Word *vaddr);
int frame_free(seL4_Word vaddr);
seL4_CPtr frame_cap(seL4_Word idx);
int sos_map_frame(seL4_Word vaddr);
int sos_unmap_frame(seL4_Word vaddr);
seL4_Word frame_paddr(seL4_Word vaddr);
bool frame_available_frames(void);

#endif
