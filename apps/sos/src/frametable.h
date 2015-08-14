#ifndef _FRAME_TABLE_H_
#define _FRAME_TABLE_H_

int frame_init(void);
int frame_alloc(seL4_Word vaddr);
void frame_free(seL4_Word vaddr);
void run_frame_tests(void);

#endif
