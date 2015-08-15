#ifndef _FRAME_TABLE_H_
#define _FRAME_TABLE_H_

int frame_init(void);
seL4_Word frame_alloc(seL4_Word *vaddr);
int frame_free(seL4_Word vaddr);

#endif
