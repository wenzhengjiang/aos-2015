#ifndef _SYSCALL_H_
#define _SYSCALL_H_

#define SYSCALL_ENDPOINT_SLOT (1)

// Syscall 1 is reserved for blocking behaviour until we can implement this
// cleanly.

#define MAX_FILENAME_LEN 255

#define SOS_SYSCALL_PRINT (2)
#define SOS_SYSCALL_BRK (3)
#define SOS_SYSCALL_USLEEP (4)
#define SOS_SYSCALL_TIMESTAMP (5)
#define SOS_SYSCALL_OPEN (6)
#define SOS_SYSCALL_READ (7)
#define SOS_SYSCALL_WRITE (8)

#endif
