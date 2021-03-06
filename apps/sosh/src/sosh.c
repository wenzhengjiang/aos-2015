/*
 * Copyright 2014, NICTA
 *
 * This software may be distributed and modified according to the terms of
 * the BSD 2-Clause license. Note that NO WARRANTY is provided.
 * See "LICENSE_BSD2.txt" for details.
 *
 * @TAG(NICTA_BSD)
 */

/* Simple shell to run on SOS */

#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/time.h>
#include <utils/time.h>
#include <stdbool.h>

/* Your OS header file */
#include <sos.h>

#define BUF_SIZ   4*1024
#define MAX_ARGS   32
#define BENCHMARK_BUF_SIZ (1048576)
#define MAX_SAMPLES 5

static char benchmark_buf[BENCHMARK_BUF_SIZ] = {0};

static int in;
static sos_stat_t sbuf;

static void prstat(const char *name) {
    /* print out stat buf */
    printf("%c%c%c%c 0x%06x 0x%lx 0x%06lx %s\n",
            sbuf.st_type == ST_SPECIAL ? 's' : '-',
            sbuf.st_fmode & FM_READ ? 'r' : '-',
            sbuf.st_fmode & FM_WRITE ? 'w' : '-',
            sbuf.st_fmode & FM_EXEC ? 'x' : '-', sbuf.st_size, sbuf.st_ctime,
            sbuf.st_atime, name);
}

static int cat(int argc, char **argv) {
    int fd;
    char buf[BUF_SIZ];
    int num_read, stdout_fd, num_written = 0;


    if (argc != 2) {
        printf("Usage: cat filename\n");
        return 1;
    }

    printf("<%s>\n", argv[1]);

    fd = open(argv[1], O_RDONLY);
    stdout_fd = open("console", O_WRONLY);

    assert(fd >= 0);

    while ((num_read = read(fd, buf, BUF_SIZ)) > 0)
        num_written = write(stdout_fd, buf, num_read);

    close(stdout_fd);

    if (num_read == -1 || num_written == -1) {
        printf("error on write\n");
        return 1;
    }

    return 0;
}

static int cp(int argc, char **argv) {
    int fd, fd_out;
    char *file1, *file2;
    char buf[BUF_SIZ];
    size_t buf_size = 1024 * 5;
    char *really_big_buf = malloc(buf_size);
    assert(really_big_buf);
    memset(really_big_buf, 0, buf_size);

    int num_read, num_written = 0;
    struct timeval start_time, end_time;

    if (argc != 3) {
        printf("Usage: cp from to\n");
        return 1;
    }

    file1 = argv[1];
    file2 = argv[2];

    fd = open(file1, O_RDONLY);
    fd_out = open(file2, O_WRONLY);

    assert(fd >= 0);
    printf("\n\n=== WRITE PERFORMANCE RESULTS ===\n");
    while ((num_read = read(fd, really_big_buf, buf_size)) > 0) {
        gettimeofday(&start_time, NULL);
        num_written = write(fd_out, really_big_buf, num_read);
        gettimeofday(&end_time, NULL);
        uint64_t elapsed = (uint64_t)((end_time.tv_sec - start_time.tv_sec) * 1000000) + (uint64_t)(end_time.tv_usec - start_time.tv_usec);
        printf("%llu ", elapsed);
    }

    if (num_read == -1 || num_written == -1) {
        printf("error on cp %d, %d\n", num_read, num_written);
        return 1;
    }

    return 0;
}

#define MAX_PROCESSES 10

static int ps(int argc, char **argv) {
    sos_process_t *process;
    int i, processes;

    process = malloc(MAX_PROCESSES * sizeof(*process));

    if (process == NULL) {
        printf("%s: out of memory\n", argv[0]);
        return 1;
    }

    processes = sos_process_status(process, MAX_PROCESSES);

    printf("TID SIZE   STIME   CTIME COMMAND\n");

    for (i = 0; i < processes; i++) {
        printf("%3x %4x %7d %s\n", process[i].pid, process[i].size,
                process[i].stime, process[i].command);
    }

    free(process);

    return 0;
}

static int exec(int argc, char **argv) {
    pid_t pid;
    int r;
    int bg = 0;

    if (argc < 2 || (argc > 2 && argv[2][0] != '&')) {
        printf("Usage: exec filename [&]\n");
        return 1;
    }

    if ((argc > 2) && (argv[2][0] == '&')) {
        bg = 1;
    }

    if (bg == 0) {
        r = close(in);
        assert(r == 0);
    }

    pid = sos_process_create(argv[1]);
    if (pid >= 0) {
        printf("Child pid=%d\n", pid);
        if (bg == 0) {
            sos_process_wait(pid);
        }
    } else {
        printf("Failed!\n");
    }
    if (bg == 0) {
        in = open("console", O_RDONLY);
        assert(in >= 0);
    }
    return 0;
}

static int dir(int argc, char **argv) {
    int i = 0, r;
    char buf[BUF_SIZ];

    if (argc > 2) {
        printf("usage: %s [file]\n", argv[0]);
        return 1;
    }

    if (argc == 2) {
        r = sos_stat(argv[1], &sbuf);
        if (r < 0) {
            printf("stat(%s) failed: %d\n", argv[1], r);
            return 0;
        }
        prstat(argv[1]);
        return 0;
    }

    while (1) {
        r = sos_getdirent(i, buf, BUF_SIZ);
        if (r < 0) {
            printf("dirent(%d) failed: %d\n", i, r);
            break;
        } else if (!r) {
            break;
        }
        r = sos_stat(buf, &sbuf);
        if (r < 0) {
            printf("stat(%s) failed: %d\n", buf, r);
            break;
        }
        prstat(buf);
        i++;
    }
    return 0;
}

const int pkg_size = 1284;
#define PKGS(n) ((n+pkg_size-1)/pkg_size)
static int benchmark(int argc,char *argv[]) {
    int max_buf_size = 1024 * 1024;
    int buf_size = 1;
    struct timeval start_time, end_time;

    if (argc != 2) {
        printf("usage: %s <mode>\n", argv[0]);
        return 1;
    }
    bool is_write = (strcmp(argv[1], "write") == 0);
    bool is_read = (strcmp(argv[1], "read") == 0);
    if (is_write)
        printf("\n\n=== WRITE PERFORMANCE RESULTS ===\n");
    else if (is_read)
        printf("\n\n=== READ PERFORMANCE RESULTS ===\n");
    else {
        printf("Unknown benchmark: %s\n",argv[1]);
        return 0;
    }
    for(buf_size = 128; buf_size <= max_buf_size; buf_size *= 2) {
        int file = open("output", O_WRONLY);
        int n = MAX_SAMPLES;
        int64_t elapsed = 0;
        int cnt = 0;
        double ave_elapsed = 0;
        int ave_cnt  = 0;
        while (n--) {
            gettimeofday(&start_time, NULL);
            cnt = is_write ? write(file, benchmark_buf, (size_t)buf_size) : read(file, benchmark_buf, (size_t)buf_size);
            gettimeofday(&end_time, NULL);
            elapsed = (uint64_t)((end_time.tv_sec - start_time.tv_sec) * 1000000) + (uint64_t)(end_time.tv_usec - start_time.tv_usec);
            printf("%d %.2lf us, %0.2lf/byte, %0.2lf/pkg\n", cnt, elapsed, (double)elapsed/cnt, (double)elapsed/PKGS(cnt));
            ave_cnt += cnt;
            ave_elapsed += elapsed;
        }
        ave_elapsed /= MAX_SAMPLES;
        ave_cnt /= MAX_SAMPLES;
        printf("ave: %d %.2lf us, %0.2lf/byte, %0.2lf/pkg\n", ave_cnt, ave_elapsed, (double)ave_elapsed/ave_cnt, (double)ave_elapsed/PKGS(ave_cnt));
        close(file);
    }
    return 0;
}


static int second_sleep(int argc,char *argv[]) {
    if (argc != 2) {
        printf("Usage %s seconds\n", argv[0]);
        return 1;
    }
    sleep(atoi(argv[1]));
    return 0;
}

static int milli_sleep(int argc,char *argv[]) {
    struct timespec tv;
    uint64_t nanos;
    if (argc != 2) {
        printf("Usage %s milliseconds\n", argv[0]);
        return 1;
    }
    nanos = (uint64_t)atoi(argv[1]) * NS_IN_MS;
    /* Get whole seconds */
    tv.tv_sec = nanos / NS_IN_S;
    /* Get nanos remaining */
    tv.tv_nsec = nanos % NS_IN_S;
    nanosleep(&tv, NULL);
    return 0;
}

static int second_time(int argc, char *argv[]) {
    printf("%d seconds since boot\n", (int)time(NULL));
    return 0;
}

static int micro_time(int argc, char *argv[]) {
    struct timeval time;
    gettimeofday(&time, NULL);
    uint64_t micros = (uint64_t)time.tv_sec * US_IN_S + (uint64_t)time.tv_usec;
    printf("%llu microseconds since boot\n", micros);
    return 0;
}

static int get_pid(int argc, char *argv[]) {
    int pid = sos_my_id();
    printf("Our pid is %d\n", pid);
    return pid;
}

static int kill(int argc, char* argv[]) {
    if (argc != 2) {
        printf("Usage %s pid\n");
        return 1;
    }
    pid_t pid = atoi(argv[1]);
    int err = sos_process_delete(pid);
    return err;
}

struct command {
    char *name;
    int (*command)(int argc, char **argv);
};


struct command commands[] = { { "dir", dir }, { "bench", benchmark }, { "ls", dir }, { "cat", cat }, {
        "cp", cp }, { "ps", ps }, { "exec", exec }, {"sleep",second_sleep}, {"msleep",milli_sleep},
                              {"time", second_time}, {"mtime", micro_time}, {"getpid", get_pid}, {"kill", kill} };


static void create_tmpfiles(void) {


}

// TODO: Remove this once satisfied all is okay.
static void m5_test(void) {
    int file, r;
    char buf[BUF_SIZ];
    file = open("readnonexistent", O_RDONLY);
    assert(file == -1);
    close(file);
    file = open("readnonexistentVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVLONGSTRINGVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVV", O_RDONLY);
    assert(file == -1);
    close(file);
    r = sos_getdirent(-1, buf, BUF_SIZ);
    assert(r == -1);

    // Small buf size
    buf[0] = 0;
    r = sos_getdirent(0, buf, 0);
    printf("r = %d\n");
    assert(buf[0] == 0);
    // No such file
    printf("%u is nbyte\n", BUF_SIZ);
    r = sos_getdirent(100000, buf, BUF_SIZ);
    assert(r == 0);
    r = sos_stat(NULL, &sbuf);
    assert(r == -1);
    file = open("tmp1", O_RDWR);
    char *msg = "hello, world";
    r = write(file, msg, strlen(msg));
    assert(r == strlen(msg));
    close(file);
    file = open("tmp1", O_RDONLY);
    assert(file >= 0);
    r = read(file, buf, BUF_SIZ);
    assert(r == strlen("hello, world"));
    r = close(file);
    file = open("bootimg.elf", O_RDONLY);
    assert(file >= 0);
    r = read(file, buf, BUF_SIZ);
    assert(r == BUF_SIZ);
    r = close(file);
    assert(r == 0);
    r = read(500, buf, BUF_SIZ);
    assert(r == -1);
    r = read(file, buf, BUF_SIZ);
    assert(r == -1);
    sos_my_id();
}

void two_coye(void) {
    int j = get_pid(0, NULL);
    printf("\n[==== proc %d starting ...=====]\n", j);
    if (j == 1) {
        char* args[3];
        args[0] = "exec";
        args[1] = "sosh";
        args[2] = "&";
        exec(3, args);
        args[0] = "cp";
        args[1] = "bootimg.elf";
        args[2] = "bootimg1.elf";
        cp(3, args);
    } else {
        char* args[3];
        args[0] = "cp";
        args[1] = "bootimg.elf";
        args[2] = "bootimg2.elf";
        cp(3, args);
    }

    printf("\n[==== proc %d exiting ...=====]\n", j);
    exit(0);
}

void build_tree(void) {
    int j = get_pid(0, NULL);
    printf("\n[==== proc %d starting ...=====]\n", j);

    if (j < 20) {
        char* args[3];
        args[0] = "exec";
        args[1] = "sosh";
        args[2] = "&";
        exec(3, args);
        exec(3, args);
    }

    printf("\n[==== proc %d exiting ...=====]\n", j);
    exit(0);
}

void multi_create_kill_test(void) {
        
    pid_t cur_pid = sos_my_id();
    printf("proc %d creating proc ...\n", cur_pid);
    pid_t child_pid = sos_process_create("sosh");
    printf("proc %d created proc %d\n", cur_pid, child_pid);
    exit(0);
}

void large_num_proc_test(int num) {
    pid_t cur_pid = sos_my_id();
    if (cur_pid < num) {
        printf("proc %d creating proc ...\n", cur_pid);
        pid_t child_pid = sos_process_create("sosh");
        printf("proc %d created proc %d\n", cur_pid, child_pid);
        while(1);
    }
}

int main(void) {
    char buf[BUF_SIZ];
    char *argv[MAX_ARGS];
    int i, r, done, found, new, argc;
    char *bp, *p;

    //large_num_proc_test(10);
    // two_coye();
    //build_tree();
    //m5_test() ;
    in = open("console", O_RDONLY);
    bp = buf;
    done = 0;
    new = 1;

//    sos_debug_print("SOS starting\n");
    printf("\n[SOS Starting]\n");

    while (!done) {
        if (new) {
            printf("$ ");
        }
        new = 0;
        found = 0;

        while (!found && !done) {
            /* Make sure to flush so anything is visible while waiting for user input */
            fflush(stdout);
            r = read(in, bp, BUF_SIZ - 1 + buf - bp);
            if (r < 0) {
                printf("Console read failed!\n");
                done = 1;
                break;
            }
            bp[r] = 0; /* terminate */
            for (p = bp; p < bp + r; p++) {
                if (*p == '\03') { /* ^C */
                    printf("^C\n");
                    p = buf;
                    new = 1;
                    break;
                } else if (*p == '\04') { /* ^D */
                    p++;
                    found = 1;
                } else if (*p == '\010' || *p == 127) {
                    /* ^H and BS and DEL */
                    if (p > buf) {
                        printf("\010 \010");
                        p--;
                        r--;
                    }
                    p--;
                    r--;
                } else if (*p == '\n') { /* ^J */
                    printf("%c", *p);
                    *p = 0;
                    found = p > buf;
                    p = buf;
                    new = 1;
                    break;
                } else {
                    printf("%c", *p);
                }
            }
            bp = p;
            if (bp == buf) {
                break;
            }
        }

        if (!found) {
            continue;
        }

        argc = 0;
        p = buf;

        while (*p != '\0') {
            /* Remove any leading spaces */
            while (*p == ' ')
                p++;
            if (*p == '\0')
                break;
            argv[argc++] = p; /* Start of the arg */
            while (*p != ' ' && *p != '\0') {
                p++;
            }

            if (*p == '\0')
                break;

            /* Null out first space */
            *p = '\0';
            p++;
        }

        if (argc == 0) {
            continue;
        }

        found = 0;

        if (strcmp(argv[0], "quit") == 0) {
            break;
        }

        for (i = 0; i < sizeof(commands) / sizeof(struct command); i++) {
            if (strcmp(argv[0], commands[i].name) == 0) {
                commands[i].command(argc, argv);
                found = 1;
                break;
            }
        }

        /* Didn't find a command */
        if (found == 0) {
            /* They might try to exec a program */
            if (sos_stat(argv[0], &sbuf) != 0) {
                printf("Command \"%s\" not found\n", argv[0]);
            } else if (!(sbuf.st_fmode & FM_EXEC)) {
                printf("File \"%s\" not executable\n", argv[0]);
            } else {
                /* Execute the program */
                argc = 2;
                argv[1] = argv[0];
                argv[0] = "exec";
                exec(argc, argv);
            }
        }
    }
    printf("[SOS Exiting]\n");
}
