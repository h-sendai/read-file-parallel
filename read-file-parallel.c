#define _GNU_SOURCE
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "drop-page-cache.h"
#include "get_num.h"

#define MAX_CHILD 1024

int pipe_to_child[MAX_CHILD][2];
int pipe_from_child[MAX_CHILD][2];
pid_t parent_pid;
struct timeval start_time;

struct {
    int debug;
    int dont_drop_pagecache;
    int use_direct_io;
    int fadv_sequential;
    int fadv_random;
    int record_time;
    long target_read_bytes;
} opts = { 0, 0, 0, 0, 0, 0, 0 };

struct time_record {
    struct timeval tv;
    int    cpu_num;
};

int usage()
{
    char msg[] = "Usage: read-file-parallel options filename [filename ...]\n"
                 "Options\n"
                 "    -d (debug)\n"
                 "    -b bufsize (64k)\n"
                 "    -i (O_DIRECT)\n"
                 "    -s (FADV_SEQUENTIAL)\n"
                 "    -r (FADV_RANDOM)\n"
                 "    -D (dont-drop-page-cache)\n"
                 "    -n total_read_bytes (exit after read this bytes)\n"
                 "If not -D option is not specified, drop page cache before read()\n"
                 "-s and -r are mutually exclusive\n";
    fprintf(stderr, "%s", msg);

    return 0;
}

off_t get_filesize(char *path)
{
    struct stat st;
    if (stat(path, &st) < 0) {
        warn("stat");
        return -1;
    }

    return st.st_size;
}

int child_proc(int proc_num, char *filename, int bufsize)
{
    int n;
    char *data_buf;
    //struct timeval *ts = NULL;
    struct time_record *time_record = NULL;

    //int index_ts = 0;
    int time_record_index = 0;

    if (close(pipe_to_child[proc_num][1]) < 0) {
        err(1, "close(pipe_to_child");
    }
    if (close(pipe_from_child[proc_num][0]) < 0) {
        err(1, "close(pipe_to_child");
    }

    /* prepare process here */
    if (opts.debug) {
        fprintf(stderr, "%d %s\n", proc_num, filename);
    }

    if (opts.use_direct_io) {
        data_buf = aligned_alloc(512, bufsize);
    }
    else {
        data_buf = malloc(bufsize);
    }
    if (data_buf == NULL) {
        err(1, "malloc for %s", filename);
    }

    if (opts.record_time) {
        off_t filesize = get_filesize(filename);
        if (filesize < 0) {
            errx(1, "get_filesize()");
        }
        int n_time_record = filesize / bufsize;
        n_time_record += 1; /* in case of filesize/bufsize if not integer */
        //ts = (struct timeval *)malloc(sizeof(struct timeval)*n_ts);
        time_record = (struct time_record *)malloc(sizeof(struct time_record)*n_time_record);
        if (time_record == NULL) {
            err(1, "malloc for ts");
        }
    };

    /* prepare done */
    /* tell ready to master process */
    char d = 'd';
    n = write(pipe_from_child[proc_num][1], &d, 1);
    if (n < 0) {
        err(1, "write from child: %d", proc_num);
    }

    /* wait from master */
    char buf[4];
    n = read(pipe_to_child[proc_num][0], buf, 1);
    if (n < 0) {
        err(1, "read on child: %d", proc_num);
    }

    /* main process here */

    int open_flags = O_RDONLY;
    if (opts.use_direct_io) {
        open_flags |= O_DIRECT;
    }
    int fd = open(filename, open_flags);
    if (fd < 0) {
        err(1, "open for %s", filename);
    }

    int advice = 0;
    if (opts.fadv_sequential) {
        advice = POSIX_FADV_SEQUENTIAL;
    }
    else if (opts.fadv_random) {
        advice = POSIX_FADV_RANDOM;
    }
    /* if more advise, write here */

    if (opts.debug) {
        fprintf(stderr, "advice: %d\n", advice);
    }

    if (advice != 0) {
        if (posix_fadvise(fd, 0, 0, advice) < 0) {
            err(1, "posix_fadvise()");
        }
    }

    struct timeval tv0, tv1, elapsed;
    gettimeofday(&tv0, NULL);
    if (opts.debug) {
        fprintf(stderr, "%ld.%06ld %d start\n", tv0.tv_sec, tv0.tv_usec, proc_num);
    }

    long total_read_bytes = 0;
    for ( ; ; ) {
        n = read(fd, data_buf, bufsize);
        if (n == 0) {
            break;
        }
        if (n < 0) {
            err(1, "read for %s", filename);
        }
        total_read_bytes += n;
        if (opts.record_time) {
            gettimeofday(&time_record[time_record_index].tv, NULL);
            time_record[time_record_index].cpu_num = sched_getcpu();
            time_record_index ++;
        }
        if (opts.target_read_bytes > 0) {
            if (total_read_bytes >= opts.target_read_bytes) {
                break;
            }
        }
    }
    gettimeofday(&tv1, NULL);
    timersub(&tv1, &tv0, &elapsed);

    double read_time = (double)elapsed.tv_sec + 0.000001*elapsed.tv_usec;
    double read_rate = (double)total_read_bytes / read_time / 1024.0 / 1024.0;
    printf("%.3f MB/s %ld bytes %ld.%06ld sec %s %d\n", 
        read_rate, total_read_bytes, elapsed.tv_sec, elapsed.tv_usec, filename, proc_num);
    fflush(stdout);

    if (opts.record_time) {
        /* tell ready to master process */
        char d = 'd';
        n = write(pipe_from_child[proc_num][1], &d, 1);
        if (n < 0) {
            err(1, "write from child: %d", proc_num);
        }

        /* wait from master */
        char buf[4];
        n = read(pipe_to_child[proc_num][0], buf, 1);
        if (n < 0) {
            err(1, "read on child: %d", proc_num);
        }
        
        /* then print timestamps */
        
        if (opts.debug) {
            fprintf(stderr, "proc_num: %d: output time_record\n", proc_num);
        }

        char output_filename[1024];
        snprintf(output_filename, sizeof(output_filename), "time.%d.%d", parent_pid, proc_num);
        FILE *fp = fopen(output_filename, "w");
        if (fp == NULL) {
            err(1, "fopen");
        }
        struct timeval elapsed;
        for (int i = 0; i < time_record_index; ++i) {
            timersub(&time_record[i].tv, &start_time, &elapsed);
            fprintf(fp, "%ld.%06ld %d %d\n", elapsed.tv_sec, elapsed.tv_usec, proc_num, time_record[i].cpu_num);
        }
    }
        
    return 0;
}

int main(int argc, char *argv[])
{
    long bufsize = 64*1024;
    int c;
    while ( (c = getopt(argc, argv, "hb:dDin:srt")) != -1) {
        switch (c) {
            case 'h':
                usage();
                exit(0);
                break;
            case 'b':
                bufsize = get_num(optarg);
                break;
            case 'd':
                opts.debug++;
                break;
            case 'D':
                opts.dont_drop_pagecache = 1;
                break;
            case 'i':
                opts.use_direct_io = 1;
                break;
            case 'n':
                opts.target_read_bytes = get_num(optarg);
                break;
            case 's':
                opts.fadv_sequential = 1;
                break;
            case 'r':
                opts.fadv_random = 1;
                break;
            case 't':
                opts.record_time = 1;
                break;
            default:
                break;
        }
    }
    argc -= optind;
    argv += optind;

    if (argc == 0) {
        usage();
        exit(1);
    }

    if (opts.fadv_sequential && opts.fadv_random) {
        fprintf(stderr, "-s and -r are exclusive\n");
        exit(1);
    }

    if (opts.debug) {
        fprintf(stderr, "argc: %d\n", argc);
    }

    /* used in record time files if record_time option is specified */
    parent_pid = getpid();

    int n_child = argc;
    if (n_child > MAX_CHILD) {
        errx(1, "max number of child processes is 1024");
    }

    if (opts.debug) {
        for (int i = 0; i < argc; ++i) {
            fprintf(stderr, "main: argc[%d]: %s\n", i, argv[i]);
        }
    }

    {
        int error_count = 0;
        for (int i = 0; i < argc; ++i) {
            /* check file readable or not */
            if (access(argv[i], R_OK) < 0) {
                warn("file: %s", argv[i]);
                error_count++;
            }
        }
        if (error_count != 0) {
            fprintf(stderr, "file read error\n");
        }
    }

    if (opts.dont_drop_pagecache) {
        ;
    }
    else {
        for (int i = 0; i < n_child; ++i) {
            if (drop_page_cache(argv[i]) < 0) {
                errx(1, "cannot drop pagecache: %s\n", argv[i]);
            }
        }
    }

    gettimeofday(&start_time, NULL);

    for (int i = 0; i < n_child; ++i) {
        if (pipe(pipe_to_child[i]) < 0) {
            err(1, "pipe_to_child");
        }
        if (pipe(pipe_from_child[i]) < 0) {
            err(1, "pipe_from_child");
        }
        
        if (opts.debug >= 2) {
            fprintf(stderr, "pipes:\n");
            fprintf(stderr, "pipe_to_child[%d][0]:     %d\n", i, pipe_to_child[i][0]);
            fprintf(stderr, "pipe_to_child[%d][1]:     %d\n", i, pipe_to_child[i][1]);
            fprintf(stderr, "pipe_from_child[%d][0]:   %d\n", i, pipe_from_child[i][0]);
            fprintf(stderr, "pipe_fromto_child[%d][1]: %d\n", i, pipe_from_child[i][1]);
        }

        pid_t pid = fork();
        if (pid == 0) { /* child */
            child_proc(i, argv[i], bufsize);
            exit(0);
        }
        else { /* parent */
            if (close(pipe_to_child[i][0]) < 0) {
                err(1, "close(pipe_to_child");
            }
            if (close(pipe_from_child[i][1]) < 0) {
                err(1, "close(pipe_to_child");
            }
            
        }
    }

    /* wait until 'done' from child */
    for (int i = 0; i < n_child; ++i) {
        char buf[4];
        int n = read(pipe_from_child[i][0], buf, 1);
        if (n < 0) {
            err(1, "read from child: %d", i);
        }
        if (opts.debug) {
            fprintf(stderr, "main: from child: %d %c\n", i, buf[0]);
        }
    }

    /* send go */
    for (int i = 0; i < n_child; ++i) {
        char g = 'g';
        int n = write(pipe_to_child[i][1], &g, 1);
        if (n < 0) {
            err(1, "write to %d", i);
        }
    }

    if (opts.record_time) {
        for (int i = 0; i < n_child; ++i) {
            char buf[4];
            int n = read(pipe_from_child[i][0], buf, 1);
            if (n < 0) {
                err(1, "read from child: %d", i);
            }
            if (opts.debug) {
                fprintf(stderr, "main: from child: %d %c\n", i, buf[0]);
            }
        }
        for (int i = 0; i < n_child; ++i) {
            char g = 'g';
            int n = write(pipe_to_child[i][1], &g, 1);
            if (n < 0) {
                err(1, "write to %d", i);
            }
        }
    }

    while (wait(NULL) > 0) {
        /* if no child, wait() return -1 with errno ECHILD: No child processes */
        ;
    }

    if (opts.debug >= 2) {
        fprintf(stderr, "parent done\n");
    }

    return 0;
}
