#ifndef _SYSCALL_H_
#define _SYSCALL_H_

#define SYSCALL_ENDPOINT_SLOT (1)

// Syscall 1 is reserved for blocking behaviour until we can implement this
// cleanly.

#define MAX_FILE_PATH_LENGTH 255

#define SOS_SYSCALL_BRK (3)
#define SOS_SYSCALL_USLEEP (4)
#define SOS_SYSCALL_TIMESTAMP (5)
#define SOS_SYSCALL_OPEN (6)
#define SOS_SYSCALL_READ (7)
#define SOS_SYSCALL_WRITE (8)
#define SOS_SYSCALL_GETDIRENT (9)
#define SOS_SYSCALL_STAT (10)
#define SOS_SYSCALL_CLOSE (11)
#define SOS_SYSCALL_PROC_CREATE (12)
#define SOS_SYSCALL_GETPID (13)
#define SOS_SYSCALL_WAITPID (14)
#define SOS_SYSCALL_PROC_DELETE (15)
#define SOS_SYSCALL_PROC_STATUS (16)

#define OPEN_MESSAGE_START (2)
#define PRINT_MESSAGE_START (2)
#define STAT_MESSAGE_START (2)
#define PROC_CREATE_MESSAGE_START (1)

#define STDIN_FD (0)
#define STDOUT_FD (1)
#define STDERR_FD (2)

#endif
