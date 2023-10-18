#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
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

int debug = 0;
int dont_drop_pagecache = 0;

int usage()
{
    char msg[] = "Usage: read-file-parallel [-b bufsize (64k)] [-D (dont-drop-page-cache)] filename [filename ...]";
    fprintf(stderr, "%s\n", msg);

    return 0;
}

int child_proc(int proc_num, char *filename, int bufsize)
{
    int n;
    char *data_buf;

    if (close(pipe_to_child[proc_num][1]) < 0) {
        err(1, "close(pipe_to_child");
    }
    if (close(pipe_from_child[proc_num][0]) < 0) {
        err(1, "close(pipe_to_child");
    }

    /* prepare process here */
    if (debug) {
        fprintf(stderr, "%d %s\n", proc_num, filename);
    }
    data_buf = malloc(bufsize);
    if (data_buf == NULL) {
        err(1, "malloc for %s", filename);
    }
    
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

    int fd = open(filename, O_RDONLY);
    if (fd < 0) {
        err(1, "open for %s", filename);
    }

    struct timeval tv0, tv1, elapsed;
    gettimeofday(&tv0, NULL);
    if (debug) {
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
    }
    gettimeofday(&tv1, NULL);
    timersub(&tv1, &tv0, &elapsed);

    double read_time = (double)elapsed.tv_sec + 0.000001*elapsed.tv_usec;
    double read_rate = (double)total_read_bytes / read_time / 1024.0 / 1024.0;
    printf("%ld.%06ld %.3f MB/s %s %d\n", 
        elapsed.tv_sec, elapsed.tv_usec, read_rate, filename, proc_num);
    fflush(stdout);

    return 0;
}

int main(int argc, char *argv[])
{
    long bufsize = 64*1024;
    int c;
    while ( (c = getopt(argc, argv, "b:dD")) != -1) {
        switch (c) {
            case 'b':
                bufsize = get_num(optarg);
                break;
            case 'd':
                debug++;
                break;
            case 'D':
                dont_drop_pagecache = 1;
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

    if (debug) {
        fprintf(stderr, "argc: %d\n", argc);
    }

    int n_child = argc;
    if (n_child > MAX_CHILD) {
        errx(1, "max number of child processes is 1024");
    }

    if (debug) {
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

    if (dont_drop_pagecache) {
        ;
    }
    else {
        for (int i = 0; i < n_child; ++i) {
            if (drop_page_cache(argv[i]) < 0) {
                errx(1, "cannot drop pagecache: %s\n", argv[i]);
            }
        }
    }

    for (int i = 0; i < n_child; ++i) {
        if (pipe(pipe_to_child[i]) < 0) {
            err(1, "pipe_to_child");
        }
        if (pipe(pipe_from_child[i]) < 0) {
            err(1, "pipe_from_child");
        }
        
        if (debug >= 2) {
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
        if (debug) {
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

    while (wait(NULL) > 0) {
        /* if no child, wait() return -1 with errno ECHILD: No child processes */
        ;
    }

    if (debug >= 2) {
        fprintf(stderr, "parent done\n");
    }

    return 0;
}
