/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2007-2008 H. Peter Anvin
 * Copyright (c) 2026 Intel Corporation; Author: H. Peter Anvin
 */

/*
 * daemon.c - "daemonize" a process
 */

#include "config.h"

int daemon(int nochdir, int noclose)
{
    int nullfd;
    pid_t f;

    if (!nochdir) {
        if (chdir("/"))
            return -1;
    }

    if (!noclose) {
        if ((nullfd = open(_PATH_DEVNULL, O_RDWR)) < 0)
            return -1;
        if (dup2(nullfd, 0) < 0 ||
            dup2(nullfd, 1) < 0 || dup2(nullfd, 2) < 0) {
            close(nullfd);
            return -1;
        }
        close(nullfd);
    }

    f = fork();
    if (f < 0)
        return -1;
    else if (f > 0)
        _exit(0);

#ifdef HAVE_SETSID
    return setsid();
#else
    return 0;
#endif
}
