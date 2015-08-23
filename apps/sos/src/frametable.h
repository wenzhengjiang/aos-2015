#ifndef _FRAME_TABLE_H_
#define _FRAME_TABLE_H_

int frame_init(void);
seL4_Word frame_alloc(seL4_Word *vaddr);
int frame_free(seL4_Word vaddr);
seL4_CPtr frame_cap(seL4_Word idx);
int sos_map_frame(seL4_Word vaddr);
int sos_unmap_frame(seL4_Word vaddr);
seL4_Word frame_paddr(seL4_Word vaddr);

#endif
